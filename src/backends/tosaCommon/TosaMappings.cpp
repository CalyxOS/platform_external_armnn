//
// Copyright © 2022 Arm Ltd and Contributors. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "TosaMappings.hpp"

using namespace armnn;
using namespace tosa;

void SetBasicBlockConstantTensorData(Layer* layer, TosaSerializationBasicBlock* /*basicBlock*/)
{
    switch (layer->GetType())
    {
        case LayerType::Convolution2d:
        {
            // ToDo: using Convolution2d as an example as it has constant tensors for weights and bias.
            // ToDo: manually set TosaOperator data of basicBlock where constant tensors exist.
        }
        default:
            // If no switch statement for layer, no constant tensors exist in that layer, return
            return;
    }
}

TosaSerializationBasicBlock* CreateEmptyTosaSerializationBasicBlock()
{
    // empty basic block when no tosa mapping implemented/exists
    TosaSerializationOperator* op =
        new TosaSerializationOperator(Op_UNKNOWN, Attribute_NONE, nullptr, {}, {});
    return new TosaSerializationBasicBlock("", {op}, {}, {}, {});
}

TosaSerializationBasicBlock* GetTosaMapping(const LayerType type,
                                            const std::vector<const TensorInfo*>& inputs,
                                            const std::vector<const TensorInfo*>& outputs,
                                            const BaseDescriptor& descriptor,
                                            bool isMain = false)
{
    switch (type)
    {
        case LayerType::Addition:
        {
            return ConvertAdditionToTosaOperator(inputs, outputs, isMain);
        }
        case LayerType::Pooling2d:
        {
            auto poolDesc = PolymorphicDowncast<const Pooling2dDescriptor*>(&descriptor);

            bool avgPoolIgnoreValue =
                (poolDesc->m_PoolType == PoolingAlgorithm::Average) &&
                (poolDesc->m_PaddingMethod == PaddingMethod::IgnoreValue);

            if (poolDesc->m_PoolType == PoolingAlgorithm::L2)
            {
                return CreateEmptyTosaSerializationBasicBlock();
            }
            else if (avgPoolIgnoreValue)
            {
                return ConvertAvgPool2DIgnoreValueToTosaOperator(inputs, outputs, isMain, poolDesc);
            }
            else
            {
                return ConvertPooling2DToTosaOperator(inputs, outputs, isMain, poolDesc);
            }
        }
        default:
        {
            return CreateEmptyTosaSerializationBasicBlock();
        }
    }
}

TosaSerializationBasicBlock* GetTosaMappingFromLayer(Layer* layer, bool isMain = false)
{
    std::vector<const TensorInfo*> inputs;
    for (auto inputSlot : layer->GetInputSlots())
    {
        inputs.push_back(&inputSlot.GetConnection()->GetTensorInfo());
    }

    std::vector<const TensorInfo*> outputs;
    for (auto& outputSlot : layer->GetOutputSlots())
    {
        outputs.push_back(&outputSlot.GetTensorInfo());
    }

    TosaSerializationBasicBlock* basicBlock = GetTosaMapping(layer->GetType(),
                                                             inputs,
                                                             outputs,
                                                             layer->GetParameters(),
                                                             isMain);
    SetBasicBlockConstantTensorData(layer, basicBlock);
    return basicBlock;
}
