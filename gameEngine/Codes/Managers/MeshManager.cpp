#include "MeshManager.h"
#include "d3dUtil.h"
#include "ResourceLoader.h"
#include "MaterialManager.h"

bool MeshManager::CreateMesh(const std::string& name, const std::wstring& filepath, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	//메시 생성
	Mesh mMesh;
	mMesh.name = name;
	//============================================================================


	//모델 로드
	MeshLoad(std::filesystem::path(filepath), mMesh.cpuModel);
	//============================================================================


	//서브메시 합체
	std::vector<Vertex> allVertices;
	std::vector<uint32_t> allIndices;

	struct SubmeshOffset
	{
		UINT indexCount;
		UINT startIndexLocation;
		UINT baseVertexLocation;
	};
	std::vector<SubmeshOffset> offsets;

	for (const auto& sub : mMesh.cpuModel.submeshes)
	{
		UINT baseVertex = (UINT)allVertices.size();
		UINT startIndex = (UINT)allIndices.size();

		// 정점 추가
		allVertices.insert(allVertices.end(), sub.vertices.begin(), sub.vertices.end());

		// 인덱스 추가 (baseVertex를 더해줘야 함)
		for (auto index : sub.indices)
		{
			allIndices.push_back(index + baseVertex);
		}

		// 오프셋 기록
		offsets.push_back({ (UINT)sub.indices.size(), startIndex, baseVertex });
	}
	//============================================================================


	mMesh.vertexCount = (UINT)allVertices.size();
	mMesh.indexCount = (UINT)allIndices.size();


	//버퍼 생성
	const UINT vbByteSize = mMesh.vertexCount * sizeof(Vertex);
	const UINT ibByteSize = mMesh.indexCount * sizeof(uint32_t);

	mMesh.vertexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, allVertices.data(), vbByteSize, mMesh.vertexUploadHeap);
	mMesh.indexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, allIndices.data(), ibByteSize, mMesh.indexUploadHeap);
	//============================================================================


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


	//각 서브메시 등록 (submesh_0, submesh_1 ...)
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		if (offsets[i].indexCount == 0) continue;

		const auto& cpuSub = mMesh.cpuModel.submeshes[i];

		// Compute local AABB for this submesh
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
		submesh.BaseVertexLocation = 0;                    // ← 여기 중요! 0으로 고정
		submesh.initMaterialName = cpuSub.materialName;
		submesh.Bounds = localBounds;  // store local AABB per submesh
		std::string key = "submesh_" + std::to_string(i);
		mMesh.DrawArgs[key] = submesh;
	}
	//============================================================================

	ResolveMeshMaterials(&mMesh);

	//해시 테이블에 메시 저장
	mMeshes.emplace(name, std::make_shared<Mesh>(std::move(mMesh)));
	//============================================================================
	return true;
}


void MeshManager::ResolveMeshMaterials(Mesh* mesh)
{
	if (!mesh) return;

	for (auto& [key, submesh] : mesh->DrawArgs)
	{
		if (!submesh.initMaterialName.empty())
		{
			auto mat = MaterialManager::Get().GetMaterial(submesh.initMaterialName);
			submesh.initMaterial = mat ? mat.get() : MaterialManager::Get().GetDefaultMaterial().get();
		}
		else
		{
			submesh.initMaterial = MaterialManager::Get().GetDefaultMaterial().get();
		}
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