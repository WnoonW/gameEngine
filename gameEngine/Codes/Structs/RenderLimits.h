#pragma once
#include <cstdint>

// 씬 오브젝트 / 간접·인스턴스 렌더 공통 상한
constexpr uint32_t kMaxSceneObjects     = 8192;
constexpr uint32_t kMaxIndirectDraws    = 32768; // 서브메시 단위 요청
constexpr uint32_t kMaxIndirectGroups   = 1024;
constexpr uint32_t kMaxInstancesPerDraw = 8192;
constexpr uint32_t kIndirectFrameCount  = 3;
