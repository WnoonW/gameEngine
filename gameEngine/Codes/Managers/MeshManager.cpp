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

	// 서브메시를 전역 버퍼에 누적 (하나의 큰 VB/IB로 합침)
	struct SubmeshOffset
	{
		UINT indexCount;
		UINT startIndexLocation;
		UINT baseVertexLocation;
	};
	std::vector<SubmeshOffset> offsets;

	for (const auto& sub : mMesh.cpuModel.submeshes)
	{
		UINT baseVertex = (UINT)sAllVertices.size();
		UINT startIndex = (UINT)sAllIndices.size();

		// 정점 추가 (전역)
		sAllVertices.insert(sAllVertices.end(), sub.vertices.begin(), sub.vertices.end());

		// 인덱스 추가 (baseVertex를 더해 pre-adjust)
		for (auto index : sub.indices)
		{
			sAllIndices.push_back(index + baseVertex);
		}

		// 오프셋 기록 (이제 global 기준)
		offsets.push_back({ (UINT)sub.indices.size(), startIndex, baseVertex });
	}
	//============================================================================

	mMesh.vertexCount = (UINT)sAllVertices.size();  // 임시, global build 후 갱신
	mMesh.indexCount = (UINT)sAllIndices.size();
	// per-mesh buffer는 생성하지 않음 (global 사용)
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

		std::wstring msg = L"  [" + std::to_wstring(i) + L"] materialName = [" + wMaterialName + L"]\n";
		OutputDebugStringW(msg.c_str());
	}
	OutputDebugStringW(L"\n========================================\n\n");
#endif


	//각 서브메시 등록 (submesh_0, submesh_1 ...)
	// offsets의 start/base 는 이미 global 기준임
	for (size_t i = 0; i < offsets.size(); ++i)
	{
		if (offsets[i].indexCount == 0) continue;

		SubmeshGeometry submesh;
		submesh.IndexCount = offsets[i].indexCount;
		submesh.StartIndexLocation = offsets[i].startIndexLocation;
		submesh.BaseVertexLocation = 0;                    // pre-adjust 했으므로 0
		submesh.materialName = mMesh.cpuModel.submeshes[i].materialName;
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
		if (!submesh.materialName.empty())
		{
			// 1. 이름으로 Material 찾기
			auto mat = MaterialManager::Get().GetMaterial(submesh.materialName);

			if (mat)
			{
				submesh.material = mat.get();           // 연결 성공
			}
			else
			{
				submesh.material = MaterialManager::Get().GetDefaultMaterial().get();
			}
		}
		else
		{
			// materialName이 비어있으면 Default
			submesh.material = MaterialManager::Get().GetDefaultMaterial().get();
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

void MeshManager::Shutdown()
{
	mMeshes.clear();
	sAllVertices.clear();
	sAllIndices.clear();
	sGlobalVertexBuffer.Reset();
	sGlobalIndexBuffer.Reset();
	sGlobalVertexUploadHeap.Reset();
	sGlobalIndexUploadHeap.Reset();
	sGlobalVertexCount = 0;
	sGlobalIndexCount = 0;
}

// static 멤버 정의
std::vector<Vertex> MeshManager::sAllVertices;
std::vector<uint32_t> MeshManager::sAllIndices;
ComPtr<ID3D12Resource> MeshManager::sGlobalVertexBuffer;
ComPtr<ID3D12Resource> MeshManager::sGlobalIndexBuffer;
ComPtr<ID3D12Resource> MeshManager::sGlobalVertexUploadHeap;
ComPtr<ID3D12Resource> MeshManager::sGlobalIndexUploadHeap;
UINT MeshManager::sGlobalVertexCount = 0;
UINT MeshManager::sGlobalIndexCount = 0;

bool MeshManager::BuildGlobalBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	if (sAllVertices.empty() || sAllIndices.empty())
	{
		OutputDebugStringW(L"[MeshManager] No geometry to build global buffers.\n");
		return false;
	}

	const UINT vbByteSize = (UINT)sAllVertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)sAllIndices.size() * sizeof(uint32_t);

	sGlobalVertexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, sAllVertices.data(), vbByteSize, sGlobalVertexUploadHeap);
	sGlobalIndexBuffer = d3dUtil::CreateDefaultBuffer(device, cmdList, sAllIndices.data(), ibByteSize, sGlobalIndexUploadHeap);

	sGlobalVertexCount = (UINT)sAllVertices.size();
	sGlobalIndexCount = (UINT)sAllIndices.size();

	// 모든 로드된 메시에 global buffer 할당 (같은 VB/IB 공유)
	for (auto& pair : mMeshes)
	{
		Mesh* m = pair.second.get();
		if (m)
		{
			m->vertexBuffer = sGlobalVertexBuffer;
			m->indexBuffer = sGlobalIndexBuffer;
			m->vertexCount = sGlobalVertexCount;
			m->indexCount = sGlobalIndexCount;
		}
	}

	OutputDebugStringW(L"[MeshManager] Global geometry buffers created successfully.\n");
	return true;
}