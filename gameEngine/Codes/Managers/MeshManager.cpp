#include "MeshManager.h"
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "MaterialManager.h"
#include <cmath>
#include <cfloat>
#include <unordered_map>
#include <algorithm>

namespace
{
	struct SubmeshOffset
	{
		UINT indexCount;
		UINT startIndexLocation;
		UINT baseVertexLocation;
	};

	struct ClusterKey
	{
		int x = 0, y = 0, z = 0;
		// Quantized normal — thin limbs: opposite sides must NOT merge (or they paper-thin)
		int nx = 0, ny = 0, nz = 0;
		bool operator==(const ClusterKey& o) const
		{
			return x == o.x && y == o.y && z == o.z
				&& nx == o.nx && ny == o.ny && nz == o.nz;
		}
	};

	struct ClusterKeyHash
	{
		size_t operator()(const ClusterKey& k) const noexcept
		{
			size_t h = (static_cast<size_t>(k.x) * 73856093u)
				^ (static_cast<size_t>(k.y) * 19349663u)
				^ (static_cast<size_t>(k.z) * 83492791u);
			h ^= (static_cast<size_t>(k.nx + 4) * 2654435761u);
			h ^= (static_cast<size_t>(k.ny + 4) * 2246822519u);
			h ^= (static_cast<size_t>(k.nz + 4) * 3266489917u);
			return h;
		}
	};

	struct ClusterAccum
	{
		XMFLOAT3 posSum{ 0, 0, 0 };
		XMFLOAT3 nrmSum{ 0, 0, 0 };
		XMFLOAT2 uvSum{ 0, 0 };
		int count = 0;
		int outIndex = -1;
	};

	// Map normal to a few bins so front/back of a leg stay separate clusters
	int QuantizeNormalAxis(float n, int bins)
	{
		// n in [-1,1] → 0..bins-1
		const float t = (n * 0.5f + 0.5f) * static_cast<float>(bins - 1);
		int q = static_cast<int>(floorf(t + 0.5f));
		if (q < 0) q = 0;
		if (q >= bins) q = bins - 1;
		return q;
	}

	void ComputeModelAabb(const Model& model, XMFLOAT3& outMin, XMFLOAT3& outMax)
	{
		outMin = { FLT_MAX, FLT_MAX, FLT_MAX };
		outMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (const auto& sub : model.submeshes)
		{
			for (const auto& v : sub.vertices)
			{
				outMin.x = (std::min)(outMin.x, v.position.x);
				outMin.y = (std::min)(outMin.y, v.position.y);
				outMin.z = (std::min)(outMin.z, v.position.z);
				outMax.x = (std::max)(outMax.x, v.position.x);
				outMax.y = (std::max)(outMax.y, v.position.y);
				outMax.z = (std::max)(outMax.z, v.position.z);
			}
		}
	}

	// Vertex clustering with normal bins:
	// - solid (no hole-punch)
	// - thin features (legs): opposite normals stay in different clusters → thickness kept
	// inflateAlongNormal: push verts out slightly so limbs don't look like sticks
	bool BuildClusteredSubmesh(
		const SubMesh& src,
		float cellSize,
		const XMFLOAT3& gridOrigin,
		int normalBins,
		float inflateAlongNormal,
		SubMesh& out)
	{
		out.materialName = src.materialName;
		out.vertices.clear();
		out.indices.clear();
		if (src.vertices.empty() || src.indices.size() < 3 || cellSize <= 1e-8f)
			return false;
		if (normalBins < 2)
			normalBins = 2;

		const float invCell = 1.f / cellSize;
		std::unordered_map<ClusterKey, ClusterAccum, ClusterKeyHash> clusters;
		clusters.reserve(src.vertices.size() / 2 + 16);

		auto makeKey = [&](const Vertex& v) -> ClusterKey
		{
			XMVECTOR nn = XMVector3Normalize(XMLoadFloat3(&v.normal));
			XMFLOAT3 n{};
			XMStoreFloat3(&n, nn);
			return ClusterKey{
				static_cast<int>(floorf((v.position.x - gridOrigin.x) * invCell)),
				static_cast<int>(floorf((v.position.y - gridOrigin.y) * invCell)),
				static_cast<int>(floorf((v.position.z - gridOrigin.z) * invCell)),
				QuantizeNormalAxis(n.x, normalBins),
				QuantizeNormalAxis(n.y, normalBins),
				QuantizeNormalAxis(n.z, normalBins)
			};
		};

		for (const auto& v : src.vertices)
		{
			auto& acc = clusters[makeKey(v)];
			acc.posSum.x += v.position.x;
			acc.posSum.y += v.position.y;
			acc.posSum.z += v.position.z;
			acc.nrmSum.x += v.normal.x;
			acc.nrmSum.y += v.normal.y;
			acc.nrmSum.z += v.normal.z;
			acc.uvSum.x += v.texcoord.x;
			acc.uvSum.y += v.texcoord.y;
			++acc.count;
		}

		out.vertices.reserve(clusters.size());
		for (auto& kv : clusters)
		{
			auto& acc = kv.second;
			const float inv = 1.f / static_cast<float>((std::max)(acc.count, 1));
			XMVECTOR n = XMVector3Normalize(XMVectorSet(
				acc.nrmSum.x * inv, acc.nrmSum.y * inv, acc.nrmSum.z * inv, 0.f));
			if (XMVector3Equal(n, XMVectorZero()))
				n = XMVectorSet(0, 1, 0, 0);

			XMVECTOR p = XMVectorSet(
				acc.posSum.x * inv, acc.posSum.y * inv, acc.posSum.z * inv, 0.f);
			// Slight outward push preserves limb bulk after averaging
			if (inflateAlongNormal > 0.f)
				p = XMVectorAdd(p, XMVectorScale(n, inflateAlongNormal));

			Vertex v{};
			XMStoreFloat3(&v.position, p);
			XMStoreFloat3(&v.normal, n);
			v.texcoord = { acc.uvSum.x * inv, acc.uvSum.y * inv };
			acc.outIndex = static_cast<int>(out.vertices.size());
			out.vertices.push_back(v);
		}

		std::vector<int> vertToCluster(src.vertices.size(), 0);
		for (size_t vi = 0; vi < src.vertices.size(); ++vi)
		{
			auto it = clusters.find(makeKey(src.vertices[vi]));
			vertToCluster[vi] = (it != clusters.end()) ? it->second.outIndex : 0;
		}

		out.indices.reserve(src.indices.size());
		for (size_t i = 0; i + 2 < src.indices.size(); i += 3)
		{
			const uint32_t ia = src.indices[i];
			const uint32_t ib = src.indices[i + 1];
			const uint32_t ic = src.indices[i + 2];
			if (ia >= vertToCluster.size() || ib >= vertToCluster.size() || ic >= vertToCluster.size())
				continue;
			const int a = vertToCluster[ia];
			const int b = vertToCluster[ib];
			const int c = vertToCluster[ic];
			if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c)
				continue;
			out.indices.push_back(static_cast<unsigned int>(a));
			out.indices.push_back(static_cast<unsigned int>(b));
			out.indices.push_back(static_cast<unsigned int>(c));
		}

		return !out.vertices.empty() && out.indices.size() >= 3;
	}

	// Fallback: solid AABB box (no holes) when clustering collapses too hard
	SubMesh MakeAabbBoxSubmesh(const XMFLOAT3& bmin, const XMFLOAT3& bmax, const std::string& matName)
	{
		SubMesh out;
		out.materialName = matName;
		const XMFLOAT3 c{
			(bmin.x + bmax.x) * 0.5f,
			(bmin.y + bmax.y) * 0.5f,
			(bmin.z + bmax.z) * 0.5f
		};
		const XMFLOAT3 e{
			(bmax.x - bmin.x) * 0.5f,
			(bmax.y - bmin.y) * 0.5f,
			(bmax.z - bmin.z) * 0.5f
		};
		// 8 corners
		const XMFLOAT3 corners[8] = {
			{ c.x - e.x, c.y - e.y, c.z - e.z },
			{ c.x + e.x, c.y - e.y, c.z - e.z },
			{ c.x + e.x, c.y + e.y, c.z - e.z },
			{ c.x - e.x, c.y + e.y, c.z - e.z },
			{ c.x - e.x, c.y - e.y, c.z + e.z },
			{ c.x + e.x, c.y - e.y, c.z + e.z },
			{ c.x + e.x, c.y + e.y, c.z + e.z },
			{ c.x - e.x, c.y + e.y, c.z + e.z },
		};
		const XMFLOAT3 faceN[6] = {
			{ 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 },
			{ 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 }
		};
		// faces as quads (two tris): -Z +Z -Y +Y -X +X
		const int faces[6][4] = {
			{ 0, 1, 2, 3 }, { 4, 7, 6, 5 },
			{ 0, 4, 5, 1 }, { 3, 2, 6, 7 },
			{ 0, 3, 7, 4 }, { 1, 5, 6, 2 }
		};
		out.vertices.reserve(24);
		out.indices.reserve(36);
		for (int f = 0; f < 6; ++f)
		{
			const int base = static_cast<int>(out.vertices.size());
			for (int i = 0; i < 4; ++i)
			{
				Vertex v{};
				v.position = corners[faces[f][i]];
				v.normal = faceN[f];
				v.texcoord = { (i == 1 || i == 2) ? 1.f : 0.f, (i >= 2) ? 1.f : 0.f };
				out.vertices.push_back(v);
			}
			out.indices.push_back(base + 0);
			out.indices.push_back(base + 1);
			out.indices.push_back(base + 2);
			out.indices.push_back(base + 0);
			out.indices.push_back(base + 2);
			out.indices.push_back(base + 3);
		}
		return out;
	}
}

bool MeshManager::UploadAndRegisterMesh(const std::string& name, Mesh& mMesh,
	ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	if (name.empty() || !device || !cmdList)
		return false;

	if (mMeshes.find(name) != mMeshes.end())
	{
		OutputDebugStringA(("[MeshManager] Mesh already exists: " + name + "\n").c_str());
		return false;
	}

	if (mMesh.cpuModel.submeshes.empty())
	{
		OutputDebugStringA(("[MeshManager] No submeshes for: " + name + "\n").c_str());
		return false;
	}

	mMesh.name = name;

	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;
	std::vector<SubmeshOffset> offsets;

	for (const auto& sub : mMesh.cpuModel.submeshes)
	{
		UINT baseVertex = (UINT)allVertices.size();
		UINT startIndex = (UINT)allIndices.size();

		allVertices.insert(allVertices.end(), sub.vertices.begin(), sub.vertices.end());

		for (auto index : sub.indices)
			allIndices.push_back(index + baseVertex);

		offsets.push_back({ (UINT)sub.indices.size(), startIndex, baseVertex });
	}

	if (allVertices.empty() || allIndices.empty())
	{
		OutputDebugStringA(("[MeshManager] Empty geometry for: " + name + "\n").c_str());
		return false;
	}

	mMesh.vertexCount = (UINT)allVertices.size();
	mMesh.indexCount = (UINT)allIndices.size();

	const UINT vbByteSize = mMesh.vertexCount * sizeof(Vertex);
	const UINT ibByteSize = mMesh.indexCount * sizeof(uint32_t);

	mMesh.vertexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, allVertices.data(), vbByteSize, mMesh.vertexUploadHeap);
	mMesh.indexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, allIndices.data(), ibByteSize, mMesh.indexUploadHeap);

#ifdef _DEBUG
	{
		char buf[256];
		sprintf_s(buf, "[MeshManager] Loaded: %s (submeshes=%zu)\n",
			name.c_str(), mMesh.cpuModel.submeshes.size());
		OutputDebugStringA(buf);
	}
#endif

	for (size_t i = 0; i < offsets.size(); ++i)
	{
		if (offsets[i].indexCount == 0) continue;

		const auto& cpuSub = mMesh.cpuModel.submeshes[i];

		DirectX::BoundingBox localBounds{};
		if (!cpuSub.vertices.empty()) {
			DirectX::XMVECTOR vMin = DirectX::XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 0);
			DirectX::XMVECTOR vMax = DirectX::XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0);
			for (const auto& vert : cpuSub.vertices) {
				DirectX::XMVECTOR p = DirectX::XMLoadFloat3(&vert.position);
				vMin = DirectX::XMVectorMin(vMin, p);
				vMax = DirectX::XMVectorMax(vMax, p);
			}
			DirectX::XMFLOAT3 minF, maxF;
			DirectX::XMStoreFloat3(&minF, vMin);
			DirectX::XMStoreFloat3(&maxF, vMax);

			localBounds.Center = {
				(minF.x + maxF.x) * 0.5f,
				(minF.y + maxF.y) * 0.5f,
				(minF.z + maxF.z) * 0.5f
			};
			localBounds.Extents = {
				(maxF.x - minF.x) * 0.5f,
				(maxF.y - minF.y) * 0.5f,
				(maxF.z - minF.z) * 0.5f
			};
		}

		SubmeshGeometry submesh;
		submesh.IndexCount = offsets[i].indexCount;
		submesh.StartIndexLocation = offsets[i].startIndexLocation;
		// 인덱스가 이미 baseVertex를 반영하므로 BaseVertexLocation은 0 고정
		submesh.BaseVertexLocation = 0;
		submesh.initMaterialName = cpuSub.materialName;
		submesh.Bounds = localBounds;
		std::string key = "submesh_" + std::to_string(i);
		mMesh.DrawArgs[key] = submesh;
	}

	ResolveMeshMaterials(&mMesh);
	mMeshes.emplace(name, std::make_shared<Mesh>(std::move(mMesh)));
	return true;
}

bool MeshManager::CreateMesh(const std::string& name, const std::wstring& filepath,
	ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	Mesh mMesh;
	MeshLoad(std::filesystem::path(filepath), mMesh.cpuModel);
	return UploadAndRegisterMesh(name, mMesh, device, cmdList);
}

bool MeshManager::CreateMeshFromGeometry(const std::string& name,
	const GeometryGenerator::MeshData& meshData,
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	const std::string& materialName)
{
	if (meshData.Vertices.empty() || meshData.Indices32.empty())
	{
		OutputDebugStringA(("[MeshManager] CreateMeshFromGeometry empty data: " + name + "\n").c_str());
		return false;
	}

	Mesh mMesh;

	SubMesh sub;
	sub.materialName = materialName;
	sub.vertices.reserve(meshData.Vertices.size());
	sub.indices.reserve(meshData.Indices32.size());

	for (const auto& v : meshData.Vertices)
	{
		Vertex out{};
		out.position = v.Position;
		out.normal = v.Normal;
		out.texcoord = v.TexC;
		sub.vertices.push_back(out);
	}

	for (uint32_t idx : meshData.Indices32)
		sub.indices.push_back(idx);

	mMesh.cpuModel.submeshes.push_back(std::move(sub));
	return UploadAndRegisterMesh(name, mMesh, device, cmdList);
}

void MeshManager::ResolveMeshMaterials(Mesh* mesh)
{
	if (!mesh) return;

	for (auto& [key, submesh] : mesh->DrawArgs)
	{
		Material* resolved = nullptr;
		if (!submesh.initMaterialName.empty())
		{
			if (auto mat = MaterialManager::Get().GetMaterial(submesh.initMaterialName))
				resolved = mat->HasValidTexture() ? mat.get() : nullptr;
		}

		if (!resolved)
		{
			if (auto def = MaterialManager::Get().GetDefaultMaterial())
				resolved = def->HasValidTexture() ? def.get() : nullptr;
		}

		// 이름 매칭/기본 머티리얼 실패 → 마젠타 디버그 머티리얼
		if (!resolved)
			resolved = MaterialManager::Get().GetMissingTextureMaterial();

		submesh.initMaterial = resolved;
	}
}


Mesh* MeshManager::GetMesh(const std::string& name) const
{
	auto it = mMeshes.find(name);
	if (it != mMeshes.end())
	{
		return it->second.get();
	}
	return nullptr;
}

std::vector<std::string> MeshManager::GetLoadedMeshNames() const
{
	std::vector<std::string> names;
	names.reserve(mMeshes.size());
	for (const auto& p : mMeshes) {
		names.push_back(p.first);
	}
	return names;
}

void MeshManager::RemoveMesh(const std::string& name)
{
	mMeshes.erase(name);
}

bool MeshManager::CreateClusteredLod(
	const std::string& srcName,
	const std::string& dstName,
	int gridCells,
	ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	bool forceRebuild)
{
	if (gridCells < 4)
		gridCells = 4;
	if (!forceRebuild && GetMesh(dstName))
		return true;
	if (forceRebuild)
		RemoveMesh(dstName);

	Mesh* src = GetMesh(srcName);
	if (!src || src->cpuModel.submeshes.empty())
		return false;

	// Count source triangles — clustering hard-edged low-poly (box/cube, ~12 tris)
	// pulls split-face verts inward per normal bin and opens edge cracks that look
	// like "transparent corners" when LOD switches in the distance.
	size_t srcTris = 0;
	for (const auto& sub : src->cpuModel.submeshes)
		srcTris += sub.indices.size() / 3;

	// Simple meshes: do not generate a clustered LOD (keep base mesh only).
	constexpr size_t kMinTrisForClusterLod = 64;
	if (srcTris < kMinTrisForClusterLod)
	{
		char buf[256];
		sprintf_s(buf,
			"[MeshManager] LOD skip %s -> %s (src tris=%zu < %zu; avoids open edges)\n",
			srcName.c_str(), dstName.c_str(), srcTris, kMinTrisForClusterLod);
		OutputDebugStringA(buf);
		return true;
	}

	XMFLOAT3 bmin{}, bmax{};
	ComputeModelAabb(src->cpuModel, bmin, bmax);
	const float ex = (std::max)(bmax.x - bmin.x, 1e-4f);
	const float ey = (std::max)(bmax.y - bmin.y, 1e-4f);
	const float ez = (std::max)(bmax.z - bmin.z, 1e-4f);
	const float maxExtent = (std::max)(ex, (std::max)(ey, ez));
	const float cellSize = maxExtent / static_cast<float>(gridCells);
	// Inflate along normal opens cracks on hard edges (each face has unique verts).
	// Only use a tiny inflate on very dense organic meshes.
	const float inflate = (srcTris >= 5000) ? (cellSize * 0.12f) : 0.0f;
	const int normalBins = 4;

	Mesh mMesh;
	size_t totalTris = 0;
	for (const auto& sub : src->cpuModel.submeshes)
	{
		SubMesh out;
		if (!BuildClusteredSubmesh(sub, cellSize, bmin, normalBins, inflate, out))
			continue;
		totalTris += out.indices.size() / 3;
		mMesh.cpuModel.submeshes.push_back(std::move(out));
	}

	// Too collapsed → single solid AABB (watertight silhouette)
	if (mMesh.cpuModel.submeshes.empty() || totalTris < 4)
	{
		mMesh.cpuModel.submeshes.clear();
		const std::string mat = src->cpuModel.submeshes[0].materialName;
		mMesh.cpuModel.submeshes.push_back(MakeAabbBoxSubmesh(bmin, bmax, mat));
		char buf[256];
		sprintf_s(buf, "[MeshManager] LOD %s -> %s (AABB box fallback, cells=%d)\n",
			srcName.c_str(), dstName.c_str(), gridCells);
		OutputDebugStringA(buf);
	}
	else
	{
		// If clustering dropped most of the surface, prefer solid AABB over a
		// swiss-cheese mesh (typical when hard-edged models collapse badly).
		const float keep = static_cast<float>(totalTris) / static_cast<float>((std::max)(srcTris, size_t(1)));
		if (keep < 0.35f && srcTris < 500)
		{
			mMesh.cpuModel.submeshes.clear();
			const std::string mat = src->cpuModel.submeshes[0].materialName;
			mMesh.cpuModel.submeshes.push_back(MakeAabbBoxSubmesh(bmin, bmax, mat));
			char buf[256];
			sprintf_s(buf,
				"[MeshManager] LOD %s -> %s (AABB; cluster kept only %.0f%% tris)\n",
				srcName.c_str(), dstName.c_str(), keep * 100.f);
			OutputDebugStringA(buf);
		}
		else
		{
			char buf[256];
			sprintf_s(buf, "[MeshManager] LOD %s -> %s (cluster cells=%d, tris~%zu)\n",
				srcName.c_str(), dstName.c_str(), gridCells, totalTris);
			OutputDebugStringA(buf);
		}
	}

	return UploadAndRegisterMesh(dstName, mMesh, device, cmdList);
}

bool MeshManager::EnsureAutoLodVariants(const std::string& baseName,
	ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
	bool forceRebuild)
{
	if (baseName.empty() || baseName.find("_lod") != std::string::npos)
		return false;
	if (!GetMesh(baseName))
		return false;

	const std::string lod1 = baseName + "_lod1";
	const std::string lod2 = baseName + "_lod2";
	// denser grids + normal bins preserve thin limbs (legs/arms)
	// lod1 moderate, lod2 coarser but still thickness-aware
	bool ok = true;
	ok &= CreateClusteredLod(baseName, lod1, 40, device, cmdList, forceRebuild);
	ok &= CreateClusteredLod(baseName, lod2, 20, device, cmdList, forceRebuild);
	return ok;
}

int MeshManager::BuildLodLevelList(const std::string& baseName, Mesh* outLevels[4], int maxLevels) const
{
	if (!outLevels || maxLevels <= 0)
		return 0;
	for (int i = 0; i < maxLevels; ++i)
		outLevels[i] = nullptr;

	Mesh* base = GetMesh(baseName);
	if (!base)
		return 0;
	outLevels[0] = base;
	int count = 1;
	if (count < maxLevels)
	{
		if (Mesh* m = GetMesh(baseName + "_lod1"))
			outLevels[count++] = m;
	}
	if (count < maxLevels)
	{
		if (Mesh* m = GetMesh(baseName + "_lod2"))
			outLevels[count++] = m;
	}
	if (count < maxLevels)
	{
		if (Mesh* m = GetMesh(baseName + "_lod3"))
			outLevels[count++] = m;
	}
	return count;
}

void MeshManager::Shutdown()
{
	mMeshes.clear();
}
