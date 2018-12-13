//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#pragma once

#include "ElementwiseBaseLayer.hpp"

namespace armnn
{

class DivisionLayer : public ElementwiseBaseLayer
{
public:
    virtual std::unique_ptr<IWorkload> CreateWorkload(const Graph& graph,
                                                      const IWorkloadFactory& factory) const override;

    DivisionLayer* Clone(Graph& graph) const override;

protected:
    DivisionLayer(const char* name);
    ~DivisionLayer() = default;
};

} // namespace
