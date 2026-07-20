#include "MeshManager.h"
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "MaterialManager.h"

namespace
{
	struct SubmeshOffset
	{
		UINT indexCount;
		UINT startIndexLocation;
		UINT baseVertexLocation;
	};
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
	OutputDebugStringW(L"\n========================================\n");
	OutputDebugStringW((L"[MeshManager] Loaded: " + std::wstring(name.begin(), name.end()) + L"\n").c_str());
	OutputDebugStringW((L"  Total SubMeshes: " + std::to_wstring(mMesh.cpuModel.submeshes.size()) + L"\n").c_str());

	for (size_t i = 0; i < mMesh.cpuModel.submeshes.size(); ++i)
	{
		const SubMesh& sub = mMesh.cpuModel.submeshes[i];

		int len = MultiByteToWideChar(CP_UTF8, 0, sub.materialName.c_str(), -1, nullptr, 0);
		std::wstring wMaterialName(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, sub.materialName.c_str(), -1, &wMaterialName[0], len);

		std::wstring msg = L"  [" + std::to_wstring(i) + L"] initMaterialName = [" + wMaterialName + L"]\n";
		OutputDebugStringW(msg.c_str());
	}
	OutputDebugStringW(L"\n========================================\n\n");
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

void MeshManager::Shutdown()
{
	mMeshes.clear();
}
