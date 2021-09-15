//
// Copyright © 2021 Arm Ltd and Contributors. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "ConstLayerMemoryOptimizerStrategy.hpp"

namespace armnn
{

std::string ConstLayerMemoryOptimizerStrategy::GetName() const
{
    return m_Name;
}

MemBlockStrategyType ConstLayerMemoryOptimizerStrategy::GetMemBlockStrategyType() const
{
    return m_MemBlockStrategyType;
}

// A IMemoryOptimizerStrategy must ensure that
// 1: All MemBlocks have been assigned to a MemBin
// 2: No MemBlock is assigned to multiple MemBins
// 3: No two MemBlocks in a MemBin overlap in the X dimension
std::vector<MemBin> ConstLayerMemoryOptimizerStrategy::Optimize(std::vector<MemBlock>& memBlocks)
{
    std::vector<MemBin> memBins;
    memBins.reserve(memBlocks.size());

    for (auto& memBlock : memBlocks)
    {
        MemBin memBin;
        memBin.m_MemSize = memBlock.m_MemSize;
        memBin.m_MemBlocks.reserve(1);
        memBlock.m_Offset = 0;
        memBin.m_MemBlocks.push_back(memBlock);
        memBins.push_back(memBin);
    }

    return memBins;
}

} // namespace armnn