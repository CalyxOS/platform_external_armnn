//
// Copyright © 2022 Arm Ltd and Contributors. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "ClUnidirectionalSequenceLstmFloatWorkload.hpp"
#include "ClWorkloadUtils.hpp"

#include <aclCommon/ArmComputeUtils.hpp>
#include <aclCommon/ArmComputeTensorUtils.hpp>

#include <armnn/utility/NumericCast.hpp>
#include <armnnUtils/Permute.hpp>
#include <cl/test/ClWorkloadFactoryHelper.hpp>
#include <backendsCommon/WorkloadUtils.hpp>

#include "cl/ClTensorHandle.hpp"

namespace
{
unsigned int CalcAclAxis(unsigned int numDimensions, unsigned int axis)
{
    return (numDimensions - axis) - 1;
}
} //namespace

namespace armnn
{
using namespace armcomputetensorutils;

ClUnidirectionalSequenceLstmFloatWorkload::ClUnidirectionalSequenceLstmFloatWorkload
    (const UnidirectionalSequenceLstmQueueDescriptor& descriptor,
     const WorkloadInfo& info,
     const arm_compute::CLCompileContext& clCompileContext)
    : FloatWorkload<UnidirectionalSequenceLstmQueueDescriptor>(descriptor, info)
{
    // Report Profiling Details
    ARMNN_REPORT_PROFILING_WORKLOAD_DESC("ClUnidirectionalSequenceLstmFloatWorkload_Construct",
                                         descriptor.m_Parameters,
                                         info,
                                         GetGuid());

    const arm_compute::ICLTensor& input = static_cast<IClTensorHandle*>(m_Data.m_Inputs[0])->GetTensor();
    arm_compute::ICLTensor& output = static_cast<IClTensorHandle*>(m_Data.m_Outputs[2])->GetTensor();

    TensorInfo inputInfo = info.m_InputTensorInfos[0];
    TensorInfo outputInfo = info.m_OutputTensorInfos[2];

    arm_compute::DataType armComputeDataType = static_cast<IClTensorHandle*>(m_Data.m_Inputs[0])->GetDataType();
    armnn::DataType armnnDataType = GetArmNNDataType(armComputeDataType);

    TensorShape inputLayerShape = static_cast<IClTensorHandle*>(m_Data.m_Inputs[0])->GetShape();
    TensorShape cellStateLayerShape = static_cast<IClTensorHandle*>(m_Data.m_Inputs[2])->GetShape();
    TensorShape outputLayerShape = static_cast<IClTensorHandle*>(m_Data.m_Outputs[2])->GetShape();

    unsigned int maxTime = m_Data.m_Parameters.m_TimeMajor ? inputLayerShape[0] : inputLayerShape[1];
    unsigned int batchSize = m_Data.m_Parameters.m_TimeMajor ? inputLayerShape[1] : inputLayerShape[0];
    unsigned int inputSize = inputLayerShape[2];
    unsigned int outputSize = outputLayerShape[2];
    unsigned int numUnits = cellStateLayerShape[1];

    const TensorShape timeMajorShapeInput({maxTime, batchSize, inputSize});
    const TensorShape timeMajorShapeOutput({maxTime, batchSize, outputSize});

    //
    // Permute: performed if Unidirectional Sequence Layer inputs/outputs are in batch major format.
    //
    if (!m_Data.m_Parameters.m_TimeMajor)
    {
        std::unique_ptr<arm_compute::CLPermute> layer(new arm_compute::CLPermute());

        TensorInfo permuteOutInfo = inputInfo;
        permuteOutInfo.SetShape(timeMajorShapeInput);
        BuildArmComputeTensor(m_PermuteFirstOut, permuteOutInfo);
        armcomputetensorutils::InitialiseArmComputeTensorEmpty(m_PermuteFirstOut);

        // Permute to time major format.
        layer->configure(clCompileContext, &input, &m_PermuteFirstOut, arm_compute::PermutationVector(0U,2U,1U));
        m_Permute1.reset(layer.release());
    }

    //
    // Split and Concat Tensors
    //
    for (unsigned int i = 0; i < maxTime; ++i)
    {
        arm_compute::CLTensor splitter_out;
        arm_compute::CLTensor concat_in;

        auto splitterTensorInfo = inputInfo;
        auto concatTensorInfo = outputInfo;
        splitterTensorInfo.SetShape({batchSize, inputSize});
        concatTensorInfo.SetShape({batchSize, outputSize});
        BuildArmComputeTensor(splitter_out, splitterTensorInfo);
        BuildArmComputeTensor(concat_in, concatTensorInfo);

        armcomputetensorutils::InitialiseArmComputeTensorEmpty(splitter_out);
        armcomputetensorutils::InitialiseArmComputeTensorEmpty(concat_in);

        // append to std::vector<arm_compute::CLTensor>
        m_SplitterOutputsTensors.push_back(std::move(splitter_out));
        m_ConcatInputsTensors.push_back(std::move(concat_in));
    }

    for (unsigned int i = 0; i < maxTime; ++i)
    {
        // append to std::vector<arm_compute::ICLTensor*>
        m_SplitterOutputs.push_back(&m_SplitterOutputsTensors[i]);
        m_ConcatInputs.push_back(&m_ConcatInputsTensors[i]);
    }

    //
    // Split
    //
    unsigned int numberDimensions = 3;
    unsigned int dimension = 0; // splitting on 0-dimension (i.e. maxTime dimension)

    if (maxTime != 1) // ACL split does not work with only one element to split.
    {
        ViewsDescriptor splitterDesc(maxTime, numberDimensions);
        unsigned int splitterDimSizes[3] = {1, batchSize, inputSize};
        for (unsigned int outputIdx = 0u; outputIdx < maxTime; ++outputIdx)
        {
            splitterDesc.SetViewOriginCoord(outputIdx, dimension, splitterDimSizes[dimension] * outputIdx);
            for (unsigned int dimIdx = 0u; dimIdx < numberDimensions; ++dimIdx)
            {
                splitterDesc.SetViewSize(outputIdx, dimIdx, splitterDimSizes[dimIdx]);
            }
        }

        std::set<unsigned int> splitAxis = ComputeSplitAxis(splitterDesc, timeMajorShapeInput);

        std::unique_ptr<arm_compute::CLSplit> split_layer(new arm_compute::CLSplit());
        unsigned int aclAxisSplit = CalcAclAxis(splitterDesc.GetNumDimensions(), *splitAxis.begin());
        if (!m_Data.m_Parameters.m_TimeMajor)
        {
            split_layer->configure(&m_PermuteFirstOut, m_SplitterOutputs, aclAxisSplit);
        }
        else
        {
            split_layer->configure(&input, m_SplitterOutputs, aclAxisSplit);
        }

        split_layer->prepare();
        m_Splitter.reset(split_layer.release());
    }

    //
    // Lstm
    //
    arm_compute::LSTMParams<arm_compute::ICLTensor> lstm_param;

    m_InputToForgetWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_InputToForgetWeightsTensor, m_Data.m_InputToForgetWeights->GetTensorInfo());

    m_InputToCellWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_InputToCellWeightsTensor, m_Data.m_InputToCellWeights->GetTensorInfo());

    m_InputToOutputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_InputToOutputWeightsTensor, m_Data.m_InputToOutputWeights->GetTensorInfo());

    m_RecurrentToForgetWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_RecurrentToForgetWeightsTensor, m_Data.m_RecurrentToForgetWeights->GetTensorInfo());

    m_RecurrentToCellWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_RecurrentToCellWeightsTensor, m_Data.m_RecurrentToCellWeights->GetTensorInfo());

    m_RecurrentToOutputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_RecurrentToOutputWeightsTensor, m_Data.m_RecurrentToOutputWeights->GetTensorInfo());

    m_ForgetGateBiasTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_ForgetGateBiasTensor, m_Data.m_ForgetGateBias->GetTensorInfo());

    m_CellBiasTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_CellBiasTensor, m_Data.m_CellBias->GetTensorInfo());

    m_OutputGateBiasTensor = std::make_unique<arm_compute::CLTensor>();
    BuildArmComputeTensor(*m_OutputGateBiasTensor, m_Data.m_OutputGateBias->GetTensorInfo());

    // for future reference: check the AndroidNN API for the logic here
    if (!m_Data.m_Parameters.m_CifgEnabled)
    {
        m_InputToInputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_InputToInputWeightsTensor, m_Data.m_InputToInputWeights->GetTensorInfo());

        m_RecurrentToInputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_RecurrentToInputWeightsTensor, m_Data.m_RecurrentToInputWeights->GetTensorInfo());

        m_CellToInputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        if (m_Data.m_CellToInputWeights != nullptr)
        {
            BuildArmComputeTensor(*m_CellToInputWeightsTensor, m_Data.m_CellToInputWeights->GetTensorInfo());
        }

        m_InputGateBiasTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_InputGateBiasTensor, m_Data.m_InputGateBias->GetTensorInfo());

        lstm_param.set_cifg_params(m_InputToInputWeightsTensor.get(),
                                   m_RecurrentToInputWeightsTensor.get(),
                                   m_Data.m_CellToInputWeights ? m_CellToInputWeightsTensor.get() : nullptr,
                                   m_InputGateBiasTensor.get());
    }

    if (m_Data.m_Parameters.m_ProjectionEnabled)
    {
        m_ProjectionWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_ProjectionWeightsTensor, m_Data.m_ProjectionWeights->GetTensorInfo());

        m_ProjectionBiasTensor = std::make_unique<arm_compute::CLTensor>();
        if (m_Data.m_ProjectionBias != nullptr)
        {
            BuildArmComputeTensor(*m_ProjectionBiasTensor, m_Data.m_ProjectionBias->GetTensorInfo());
        }

        lstm_param.set_projection_params(m_ProjectionWeightsTensor.get(),
                                         m_Data.m_ProjectionBias ? m_ProjectionBiasTensor.get() : nullptr);
    }

    if (m_Data.m_Parameters.m_PeepholeEnabled)
    {
        m_CellToForgetWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_CellToForgetWeightsTensor, m_Data.m_CellToForgetWeights->GetTensorInfo());

        m_CellToOutputWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_CellToOutputWeightsTensor, m_Data.m_CellToOutputWeights->GetTensorInfo());

        lstm_param.set_peephole_params(m_CellToForgetWeightsTensor.get(), m_CellToOutputWeightsTensor.get());
    }

    if (m_Data.m_Parameters.m_LayerNormEnabled)
    {
        m_InputLayerNormWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        if (!m_Data.m_Parameters.m_CifgEnabled)
        {
            BuildArmComputeTensor(*m_InputLayerNormWeightsTensor, m_Data.m_InputLayerNormWeights->GetTensorInfo());
        }

        m_ForgetLayerNormWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_ForgetLayerNormWeightsTensor, m_Data.m_ForgetLayerNormWeights->GetTensorInfo());

        m_CellLayerNormWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_CellLayerNormWeightsTensor, m_Data.m_CellLayerNormWeights->GetTensorInfo());

        m_OutputLayerNormWeightsTensor = std::make_unique<arm_compute::CLTensor>();
        BuildArmComputeTensor(*m_OutputLayerNormWeightsTensor, m_Data.m_OutputLayerNormWeights->GetTensorInfo());

        auto inputNormWeightTensor = m_Data.m_Parameters.m_CifgEnabled ? nullptr : m_InputLayerNormWeightsTensor.get();
        lstm_param.set_layer_normalization_params(inputNormWeightTensor,
                                                  m_ForgetLayerNormWeightsTensor.get(),
                                                  m_CellLayerNormWeightsTensor.get(),
                                                  m_OutputLayerNormWeightsTensor.get());
    }

    arm_compute::ICLTensor& output_state_in = static_cast<IClTensorHandle*>(m_Data.m_Inputs[1])->GetTensor();
    arm_compute::ICLTensor& cell_state_in   = static_cast<IClTensorHandle*>(m_Data.m_Inputs[2])->GetTensor();

    arm_compute::ICLTensor& output_state_out = static_cast<IClTensorHandle*>(m_Data.m_Inputs[1])->GetTensor();
    arm_compute::ICLTensor& cell_state_out = static_cast<IClTensorHandle*>(m_Data.m_Inputs[2])->GetTensor();

    m_ScratchBuffer = std::make_unique<arm_compute::CLTensor>();
    if (m_Data.m_Parameters.m_CifgEnabled)
    {
        // scratch_buffer [num_units * 3, batch_size] with CIFG
        BuildArmComputeTensor(*m_ScratchBuffer, TensorInfo({batchSize, numUnits * 3}, armnnDataType));
    }
    else
    {
        // scratch_buffer [num_units * 4, batch_size] without CIFG
        BuildArmComputeTensor(*m_ScratchBuffer, TensorInfo({batchSize, numUnits * 4}, armnnDataType));
    }

    // Need to be set at negative threshold to be compatible for ACL
    float cell_threshold       = m_Data.m_Parameters.m_ClippingThresCell;
    float projection_threshold = m_Data.m_Parameters.m_ClippingThresProj;

    // For preparing the object for the class ActivationLayerInfo, consider 5 situations
    arm_compute::ActivationLayerInfo activationLayerInfo =
        ConvertLstmActivationFuncToAclLayerInfo(m_Data.m_Parameters.m_ActivationFunc);

    for (unsigned int i = 0; i != maxTime; ++i)
    {
        // Set LSTM input and output ITensors depending on:
        // input format (timeMajor) & number of LSTM batches (maxTime).
        arm_compute::ICLTensor* outputLSTM;
        arm_compute::ICLTensor* inputLSTM;
        // If there is only one LSTM time major batch, we will not concat OR permute.
        // Set input of LSTM to be first input ITensor.
        // Set output of LSTM to be final output ITensor.
        // LSTM input/output cannot be > 2 dimensions so need to resize its TensorInfo.
        if (maxTime == 1 && m_Data.m_Parameters.m_TimeMajor)
        {
            TensorShape inputShape = GetTensorShape((&input)->info()->tensor_shape(), 1U);
            TensorShape outputShape = GetTensorShape((&output)->info()->tensor_shape(), 1U);
            TensorShape inputShapeShrink({inputShape[1], inputShape[2]});
            TensorShape outputShapeShrink({outputShape[1], outputShape[2]});
            auto acl_input_shape_shrink = BuildArmComputeTensorShape(inputShapeShrink);
            auto acl_output_shape_shrink = BuildArmComputeTensorShape(outputShapeShrink);
            (&input)->info()->set_tensor_shape(acl_input_shape_shrink);
            inputLSTM = const_cast<arm_compute::ICLTensor*>(&input);
            (&output)->info()->set_tensor_shape(acl_output_shape_shrink);
            outputLSTM = &output;
        }
            // If there is only one LSTM batch major batch, we will not concat, only permute.
            // Set input of LSTM to be output of initial permute.
            // Set output of LSTM to be first element of m_ConcatInputs & use that value later in permute.
            // LSTM output cannot be > 2 dimensions so need to resize its TensorInfo.
        else if (maxTime == 1 && !m_Data.m_Parameters.m_TimeMajor)
        {
            TensorShape inputShape = GetTensorShape(m_PermuteFirstOut.info()->tensor_shape(), 1U);
            TensorShape inputShapeShrink({inputShape[1], inputShape[2]});
            auto acl_input_shape_shrink = BuildArmComputeTensorShape(inputShapeShrink);
            m_PermuteFirstOut.info()->set_tensor_shape(acl_input_shape_shrink);
            inputLSTM = &m_PermuteFirstOut;
            outputLSTM = const_cast<arm_compute::ICLTensor*>(m_ConcatInputs[i]);
        }
            // Batch major AND/OR 2+ LSTM batches so will use concat AND/OR permute later on.
        else
        {
            inputLSTM = m_SplitterOutputs[i];
            outputLSTM = const_cast<arm_compute::ICLTensor*>(m_ConcatInputs[i]);
        }

        std::unique_ptr<arm_compute::CLLSTMLayer> lstm_layer(new arm_compute::CLLSTMLayer());
        lstm_layer->configure(clCompileContext,
                              inputLSTM,
                              m_InputToForgetWeightsTensor.get(),
                              m_InputToCellWeightsTensor.get(),
                              m_InputToOutputWeightsTensor.get(),
                              m_RecurrentToForgetWeightsTensor.get(),
                              m_RecurrentToCellWeightsTensor.get(),
                              m_RecurrentToOutputWeightsTensor.get(),
                              m_ForgetGateBiasTensor.get(),
                              m_CellBiasTensor.get(),
                              m_OutputGateBiasTensor.get(),
                              &output_state_in,
                              &cell_state_in,
                              m_ScratchBuffer.get(),
                              &output_state_out,
                              &cell_state_out,
                              outputLSTM,
                              lstm_param,
                              activationLayerInfo,
                              cell_threshold,
                              projection_threshold);

        m_Layers.emplace_back(std::move(lstm_layer));
    }

    armcomputetensorutils::InitialiseArmComputeTensorEmpty(*m_ScratchBuffer);

    InitializeArmComputeClTensorData(*m_InputToForgetWeightsTensor, m_Data.m_InputToForgetWeights);
    InitializeArmComputeClTensorData(*m_InputToCellWeightsTensor, m_Data.m_InputToCellWeights);
    InitializeArmComputeClTensorData(*m_InputToOutputWeightsTensor, m_Data.m_InputToOutputWeights);
    InitializeArmComputeClTensorData(*m_RecurrentToForgetWeightsTensor, m_Data.m_RecurrentToForgetWeights);
    InitializeArmComputeClTensorData(*m_RecurrentToCellWeightsTensor, m_Data.m_RecurrentToCellWeights);
    InitializeArmComputeClTensorData(*m_RecurrentToOutputWeightsTensor, m_Data.m_RecurrentToOutputWeights);
    InitializeArmComputeClTensorData(*m_ForgetGateBiasTensor, m_Data.m_ForgetGateBias);
    InitializeArmComputeClTensorData(*m_CellBiasTensor, m_Data.m_CellBias);
    InitializeArmComputeClTensorData(*m_OutputGateBiasTensor, m_Data.m_OutputGateBias);

    if (!m_Data.m_Parameters.m_CifgEnabled)
    {
        InitializeArmComputeClTensorData(*m_InputToInputWeightsTensor, m_Data.m_InputToInputWeights);
        InitializeArmComputeClTensorData(*m_RecurrentToInputWeightsTensor, m_Data.m_RecurrentToInputWeights);
        if (m_Data.m_CellToInputWeights != nullptr)
        {
            InitializeArmComputeClTensorData(*m_CellToInputWeightsTensor, m_Data.m_CellToInputWeights);
        }
        InitializeArmComputeClTensorData(*m_InputGateBiasTensor, m_Data.m_InputGateBias);
    }

    if (m_Data.m_Parameters.m_ProjectionEnabled)
    {
        InitializeArmComputeClTensorData(*m_ProjectionWeightsTensor, m_Data.m_ProjectionWeights);
        if (m_Data.m_ProjectionBias != nullptr)
        {
            InitializeArmComputeClTensorData(*m_ProjectionBiasTensor, m_Data.m_ProjectionBias);
        }
    }

    if (m_Data.m_Parameters.m_PeepholeEnabled)
    {
        InitializeArmComputeClTensorData(*m_CellToForgetWeightsTensor, m_Data.m_CellToForgetWeights);
        InitializeArmComputeClTensorData(*m_CellToOutputWeightsTensor, m_Data.m_CellToOutputWeights);
    }

    if (m_Data.m_Parameters.m_LayerNormEnabled)
    {
        if (!m_Data.m_Parameters.m_CifgEnabled)
        {
            InitializeArmComputeClTensorData(*m_InputLayerNormWeightsTensor, m_Data.m_InputLayerNormWeights);
        }
        InitializeArmComputeClTensorData(*m_ForgetLayerNormWeightsTensor, m_Data.m_ForgetLayerNormWeights);
        InitializeArmComputeClTensorData(*m_CellLayerNormWeightsTensor, m_Data.m_CellLayerNormWeights);
        InitializeArmComputeClTensorData(*m_OutputLayerNormWeightsTensor, m_Data.m_OutputLayerNormWeights);
    }

    // Force Compute Library to perform the necessary copying and reshaping.
    // After which delete all the input tensors that will no longer be needed.
    for (uint32_t i = 0; i < m_Layers.size(); ++i)
    {
        m_Layers[i]->prepare();
    }

    //
    // Concat
    //

    // Expand dimensions of LSTM outputs adding one empty dimension to fit concatenate inputs.
    TensorShape shape = GetTensorShape(m_ConcatInputs[0]->info()->tensor_shape(), 1U);
    TensorShape shapeExpandTimeMajor({1, shape[0], shape[1]});
    TensorShape shapeExpandBatchMajor({shape[0], 1, shape[1]});

    if (maxTime != 1) // ACL concat does not work with only one element to concatenate.
    {
        for (unsigned int i = 0; i < maxTime; ++i)
        {
            m_ConcatInputs[i]->info()->set_tensor_shape(BuildArmComputeTensorShape(shapeExpandTimeMajor));
        }

        ConcatDescriptor  concatDescriptor(maxTime, numberDimensions);  // maxTime = num inputs (aka. number of views).
        for (unsigned int inputIdx = 0u; inputIdx < maxTime; ++inputIdx)
        {
            concatDescriptor.SetViewOriginCoord(inputIdx, dimension, inputIdx);
            concatDescriptor.SetConcatAxis(dimension);
        }

        m_Concat.reset(new arm_compute::CLConcatenateLayer());
        unsigned int aclAxisConcat = CalcAclAxis(concatDescriptor.GetNumDimensions(),
                                                 concatDescriptor.GetConcatAxis());
        if (!m_Data.m_Parameters.m_TimeMajor)
        {
            TensorInfo concatOuputTensorInfo = outputInfo;
            concatOuputTensorInfo.SetShape(timeMajorShapeOutput);
            BuildArmComputeTensor(concat_out, concatOuputTensorInfo);
            armcomputetensorutils::InitialiseArmComputeTensorEmpty(concat_out);

            m_Concat->configure(m_ConcatInputs, &concat_out, aclAxisConcat);
        }
        else
        {
            m_Concat->configure(m_ConcatInputs, &output, aclAxisConcat);
        }

        m_Concat->prepare();
    }
    // If only one LSTM batch, we do not concat and/or permute.
    // Must ensure final output info is expanded to correct batch major dimensions.
    else
    {
        if (!m_Data.m_Parameters.m_TimeMajor)
        {
            (&output)->info()->set_tensor_shape(BuildArmComputeTensorShape(shapeExpandBatchMajor));
        }
        else
        {
            (&output)->info()->set_tensor_shape(BuildArmComputeTensorShape(shapeExpandTimeMajor));
        }
    }

    //
    // Permute: only done if input/output are in batch major format.
    //
    if (!m_Data.m_Parameters.m_TimeMajor)
    {
        // Output now time major. Permute output back to batch major.
        std::unique_ptr<arm_compute::CLPermute> layer(new arm_compute::CLPermute());
        if (maxTime != 1)
        {
            layer->configure(clCompileContext, &concat_out, &output, arm_compute::PermutationVector(0U, 2U, 1U));
        }
        else
        {
            layer->configure(clCompileContext, m_ConcatInputs[0], &output, arm_compute::PermutationVector(0U, 2U, 1U));
        }
        m_Permute2.reset(layer.release());
    }

    FreeUnusedTensors();
}

void ClUnidirectionalSequenceLstmFloatWorkload::Execute() const
{
    ARMNN_SCOPED_PROFILING_EVENT_CL_GUID("ClUnidirectionalSequenceLstmFloatWorkload_Execute", GetGuid());
    if (m_Permute1)
    {
        m_Permute1->run();
    }
    if (m_Splitter)
    {
        m_Splitter->run();
    }
    for (uint32_t i = 0; i < m_Layers.size(); ++i)
    {
        m_Layers[i]->run();
    }
    if (m_Concat)
    {
        m_Concat->run();
    }
    if (m_Permute2)
    {
        m_Permute2->run();
    }
}

arm_compute::Status
ClUnidirectionalSequenceLstmFloatWorkloadValidate(const TensorInfo& input,
                                                  const TensorInfo& outputStateIn,
                                                  const TensorInfo& cellStateIn,
                                                  const TensorInfo& output,
                                                  const Optional<TensorInfo>& hiddenStateOutput,
                                                  const Optional<TensorInfo>& cellStateOutput,
                                                  const UnidirectionalSequenceLstmDescriptor& descriptor,
                                                  const LstmInputParamsInfo& paramsInfo)
{
    IgnoreUnused(hiddenStateOutput, cellStateOutput);

    TensorShape inputLayerShape  = input.GetShape();
    TensorShape outputLayerShape = outputStateIn.GetShape();

    unsigned int maxTime    = descriptor.m_TimeMajor?inputLayerShape[0]:inputLayerShape[1];
    unsigned int batchSize  = descriptor.m_TimeMajor?inputLayerShape[1]:inputLayerShape[0];
    unsigned int inputSize  = inputLayerShape[2];
    unsigned int outputSize = outputLayerShape[2];

    const TensorShape timeMajorShapeInput({maxTime, batchSize, inputSize});
    const TensorShape timeMajorShapeOutput({maxTime, batchSize, outputSize});

    arm_compute::Status statusPermute1 = arm_compute::Status(arm_compute::ErrorCode::OK,
                                                             "Permute1 status");
    arm_compute::Status statusSplit    = arm_compute::Status(arm_compute::ErrorCode::OK,
                                                             "Split status");
    arm_compute::Status statusLSTM     = arm_compute::Status(arm_compute::ErrorCode::OK,
                                                             "LSTM status");
    arm_compute::Status statusConcat   = arm_compute::Status(arm_compute::ErrorCode::OK,
                                                             "Concat status");
    arm_compute::Status statusPermute2 = arm_compute::Status(arm_compute::ErrorCode::OK,
                                                             "Permute2 status");

    const arm_compute::TensorInfo aclInputInfo  = armcomputetensorutils::BuildArmComputeTensorInfo(input);
    const arm_compute::TensorInfo aclOutputInfo = armcomputetensorutils::BuildArmComputeTensorInfo(output);

    //
    // Permute validate
    //
    TensorInfo              permuteOutInfo    = TensorInfo(input);
    arm_compute::TensorInfo aclPermuteOutInfo = armcomputetensorutils::BuildArmComputeTensorInfo(permuteOutInfo);
    if (!descriptor.m_TimeMajor)
    {
        statusPermute1 = arm_compute::CLPermute::validate(&aclInputInfo,
                                                          &aclPermuteOutInfo,
                                                          arm_compute::PermutationVector(0U, 2U, 1U));
    }

    //
    // Split and Concat Tensors validate
    //
    std::vector<arm_compute::TensorInfo>         splitterOutputsTensorInfos;
    std::vector<arm_compute::TensorInfo>         concatInputsTensorInfos;
    std::vector<arm_compute::ITensorInfo*>       splitterOutputsTensorInfosPtr;
    std::vector<const arm_compute::ITensorInfo*> concatInputsTensorInfosPtr;
    splitterOutputsTensorInfos.reserve(maxTime);
    concatInputsTensorInfos.reserve(maxTime);
    for (unsigned int i = 0; i < maxTime; ++i)
    {
        arm_compute::TensorInfo splitter_out;
        arm_compute::TensorInfo concat_in;

        auto splitterTensorInfo = TensorInfo(input);
        auto concatTensorInfo   = TensorInfo(output);
        splitterTensorInfo.SetShape({batchSize, inputSize});
        concatTensorInfo.SetShape({batchSize, outputSize});

        arm_compute::TensorInfo aclSplitterTensorInfo
                                    = armcomputetensorutils::BuildArmComputeTensorInfo(splitterTensorInfo);
        arm_compute::TensorInfo aclConcatTensorInfo
                                    = armcomputetensorutils::BuildArmComputeTensorInfo(concatTensorInfo);

        splitterOutputsTensorInfos.emplace_back(aclSplitterTensorInfo);
        concatInputsTensorInfos.emplace_back(aclConcatTensorInfo);
        splitterOutputsTensorInfosPtr.emplace_back(&splitterOutputsTensorInfos[i]);
        concatInputsTensorInfosPtr.emplace_back(&concatInputsTensorInfos[i]);
    }

    //
    // Split validate
    //
    unsigned int numberDimensions = 3;
    unsigned int dimension        = 0; // splitting on 0-dimension (i.e. maxTime dimension)
    unsigned int aclAxisSplit     = CalcAclAxis(numberDimensions, dimension);

    if (maxTime != 1) // ACL split does not work with only one element to split.
    {
        if (!descriptor.m_TimeMajor)
        {
            statusSplit = arm_compute::CLSplit::validate(&aclPermuteOutInfo,
                                                         splitterOutputsTensorInfosPtr,
                                                         aclAxisSplit);
        }
        else
        {
            statusSplit = arm_compute::CLSplit::validate(&aclInputInfo, splitterOutputsTensorInfosPtr, aclAxisSplit);
        }
    }

    //
    // LSTM validate
    //

    arm_compute::LSTMParams<arm_compute::ITensorInfo> lstm_params_info;

    const TensorInfo& scratchBuffer = TensorInfo(cellStateIn.GetShape(), input.GetDataType());
    const TensorInfo& outputStateOut = TensorInfo(outputStateIn.GetShape(), input.GetDataType());
    const TensorInfo& cellStateOut = TensorInfo(cellStateIn.GetShape(), input.GetDataType());

    // The inputs and outputs
    const arm_compute::TensorInfo aclOutputStateInInfo = BuildArmComputeTensorInfo(outputStateIn);
    const arm_compute::TensorInfo aclCellStateInInfo = BuildArmComputeTensorInfo(cellStateIn);
    const arm_compute::TensorInfo aclScratchBufferInfo = BuildArmComputeTensorInfo(scratchBuffer);
    const arm_compute::TensorInfo aclOutputStateOutInfo = BuildArmComputeTensorInfo(outputStateOut);
    const arm_compute::TensorInfo aclCellStateOutInfo = BuildArmComputeTensorInfo(cellStateOut);

    // Basic parameters
    const arm_compute::TensorInfo aclInputToForgetWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetInputToForgetWeights());
    const arm_compute::TensorInfo aclInputToCellWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetInputToCellWeights());
    const arm_compute::TensorInfo aclInputToOutputWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetInputToOutputWeights());
    const arm_compute::TensorInfo aclRecurrentToForgetWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetRecurrentToForgetWeights());
    const arm_compute::TensorInfo aclRecurrentToCellWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetRecurrentToCellWeights());
    const arm_compute::TensorInfo aclRecurrentToOutputWeightsInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetRecurrentToOutputWeights());
    const arm_compute::TensorInfo aclForgetGateBiasInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetForgetGateBias());
    const arm_compute::TensorInfo aclCellBiasInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetCellBias());
    const arm_compute::TensorInfo aclOutputGateBiasInfo
                                      = BuildArmComputeTensorInfo(paramsInfo.GetOutputGateBias());

    arm_compute::TensorInfo aclInputToInputWeightsInfo;
    arm_compute::TensorInfo aclRecurrentToInputWeightsInfo;
    arm_compute::TensorInfo aclCellToInputWeightsInfo;
    arm_compute::TensorInfo aclInputGateBiasInfo;
    arm_compute::TensorInfo aclProjectionWeightsInfo;
    arm_compute::TensorInfo aclProjectionBiasInfo;
    arm_compute::TensorInfo aclCellToForgetWeightsInfo;
    arm_compute::TensorInfo aclCellToOutputWeightsInfo;

    arm_compute::TensorInfo aclInputLayerNormWeightsInfo;
    arm_compute::TensorInfo aclForgetLayerNormWeightsInfo;
    arm_compute::TensorInfo aclCellLayerNormWeightsInfo;
    arm_compute::TensorInfo aclOutputLayerNormWeightsInfo;


    if (!descriptor.m_CifgEnabled)
    {
        if (descriptor.m_PeepholeEnabled)
        {
            aclCellToInputWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetCellToInputWeights());
        }
        aclInputToInputWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetInputToInputWeights());
        aclRecurrentToInputWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetRecurrentToInputWeights());
        aclInputGateBiasInfo = BuildArmComputeTensorInfo(paramsInfo.GetInputGateBias());

        lstm_params_info.set_cifg_params(&aclInputToInputWeightsInfo,
                                         &aclRecurrentToInputWeightsInfo,
                                         descriptor.m_PeepholeEnabled ? &aclCellToInputWeightsInfo : nullptr,
                                         &aclInputGateBiasInfo);
    }

    if (descriptor.m_ProjectionEnabled)
    {
        if (paramsInfo.m_ProjectionBias != nullptr)
        {
            aclProjectionBiasInfo = BuildArmComputeTensorInfo(paramsInfo.GetProjectionBias());
        }
        aclProjectionWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetProjectionWeights());

        lstm_params_info.set_projection_params(&aclProjectionWeightsInfo,
                                               paramsInfo.m_ProjectionBias ? &aclProjectionBiasInfo : nullptr);
    }

    if (descriptor.m_PeepholeEnabled)
    {
        aclCellToForgetWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetCellToForgetWeights());
        aclCellToOutputWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetCellToOutputWeights());

        lstm_params_info.set_peephole_params(&aclCellToForgetWeightsInfo, &aclCellToOutputWeightsInfo);
    }

    if (descriptor.m_LayerNormEnabled)
    {
        if (!descriptor.m_CifgEnabled)
        {
            aclInputLayerNormWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetInputLayerNormWeights());
        }
        aclForgetLayerNormWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetForgetLayerNormWeights());
        aclCellLayerNormWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetCellLayerNormWeights());
        aclOutputLayerNormWeightsInfo = BuildArmComputeTensorInfo(paramsInfo.GetOutputLayerNormWeights());

        lstm_params_info.set_layer_normalization_params(descriptor.m_CifgEnabled ? nullptr :
                                                        &aclInputLayerNormWeightsInfo,
                                                        &aclForgetLayerNormWeightsInfo,
                                                        &aclCellLayerNormWeightsInfo,
                                                        &aclOutputLayerNormWeightsInfo);
    }

    // Need to be set at negative threshold to be compatible for ACL
    float cell_threshold = descriptor.m_ClippingThresCell;
    float projection_threshold = descriptor.m_ClippingThresProj;

    arm_compute::ActivationLayerInfo activationLayerInfo =
        ConvertLstmActivationFuncToAclLayerInfo(descriptor.m_ActivationFunc);

    for (unsigned int i = 0; i != maxTime; ++i)
    {

        // Set LSTM input and output ITensors depending on:
        // input format (timeMajor) & number of LSTM batches (maxTime).
        arm_compute::ITensorInfo* outputLSTM;
        arm_compute::ITensorInfo* inputLSTM;
        // If there is only one LSTM time major batch, we will not concat OR permute.
        // Set input of LSTM to be first input ITensor.
        // Set output of LSTM to be final output ITensor.
        // LSTM input/output cannot be > 2 dimensions so need to resize its TensorInfo.
        if (maxTime == 1 && !descriptor.m_TimeMajor)
        {
            TensorShape inputShape = GetTensorShape(aclInputInfo.tensor_shape(), 1U);
            TensorShape outputShape = GetTensorShape(aclOutputInfo.tensor_shape(), 1U);
            TensorShape inputShapeShrink({inputShape[1], inputShape[2]});
            TensorShape outputShapeShrink({outputShape[1], outputShape[2]});
            auto acl_input_shape_shrink = BuildArmComputeTensorShape(inputShapeShrink);
            auto acl_output_shape_shrink = BuildArmComputeTensorShape(outputShapeShrink);
            const_cast<arm_compute::TensorInfo*>(&aclInputInfo)->set_tensor_shape(acl_input_shape_shrink);
            inputLSTM = const_cast<arm_compute::TensorInfo*>(&aclInputInfo);
            const_cast<arm_compute::TensorInfo*>(&aclOutputInfo)->set_tensor_shape(acl_output_shape_shrink);
            outputLSTM = const_cast<arm_compute::TensorInfo*>(&aclOutputInfo);
        }
            // If there is only one LSTM batch major batch, we will not concat, only permute.
            // Set input of LSTM to be output of initial permute.
            // Set output of LSTM to be first element of m_ConcatInputs & use that value later in permute.
            // LSTM output cannot be > 2 dimensions so need to resize its TensorInfo.
        else if (maxTime == 1 && !descriptor.m_TimeMajor)
        {
            TensorShape inputShape = GetTensorShape(aclPermuteOutInfo.tensor_shape(), 1U);
            TensorShape inputShapeShrink({inputShape[1], inputShape[2]});
            auto acl_input_shape_shrink = BuildArmComputeTensorShape(inputShapeShrink);
            aclPermuteOutInfo.set_tensor_shape(acl_input_shape_shrink);
            inputLSTM = &aclPermuteOutInfo;
            outputLSTM = const_cast<arm_compute::ITensorInfo*>(concatInputsTensorInfosPtr[i]);
        }
            // Batch major AND/OR 2+ LSTM batches so will use concat AND/OR permute later on.
        else
        {
            inputLSTM = splitterOutputsTensorInfosPtr[i];
            outputLSTM = const_cast<arm_compute::ITensorInfo*>(concatInputsTensorInfosPtr[i]);
        }

        statusLSTM = arm_compute::CLLSTMLayer::validate(inputLSTM,
                                                        &aclInputToForgetWeightsInfo,
                                                        &aclInputToCellWeightsInfo,
                                                        &aclInputToOutputWeightsInfo,
                                                        &aclRecurrentToForgetWeightsInfo,
                                                        &aclRecurrentToCellWeightsInfo,
                                                        &aclRecurrentToOutputWeightsInfo,
                                                        &aclForgetGateBiasInfo,
                                                        &aclCellBiasInfo,
                                                        &aclOutputGateBiasInfo,
                                                        &aclOutputStateInInfo,
                                                        &aclCellStateInInfo,
                                                        &aclScratchBufferInfo,
                                                        &aclOutputStateOutInfo,
                                                        &aclCellStateOutInfo,
                                                        outputLSTM,
                                                        lstm_params_info,
                                                        activationLayerInfo,
                                                        cell_threshold,
                                                        projection_threshold);

        if (statusLSTM.error_code() != arm_compute::ErrorCode::OK)
        {
            break;
        }
    }

    //
    // Concat validate
    //

    // Expand dimensions of LSTM outputs adding one empty dimension to fit concatenate inputs.
    TensorShape shape = GetTensorShape(concatInputsTensorInfosPtr[0]->tensor_shape(), 1U);
    TensorShape shapeExpandTimeMajor({1, shape[0], shape[1]});
    TensorShape shapeExpandBatchMajor({shape[0], 1, shape[1]});

    TensorInfo concatOuputTensorInfo = TensorInfo(output);
    concatOuputTensorInfo.SetShape(timeMajorShapeOutput);
    arm_compute::TensorInfo aclConcatOuputTensorInfo= BuildArmComputeTensorInfo(concatOuputTensorInfo);

    if (maxTime != 1) // ACL concat does not work with only one element to concatenate.
    {
        for (unsigned int i = 0; i < maxTime; ++i)
        {
            auto acl_shape_expand = BuildArmComputeTensorShape(shapeExpandTimeMajor);
            concatInputsTensorInfos[i].set_tensor_shape(acl_shape_expand);
        }

        unsigned int aclAxisConcat = CalcAclAxis(numberDimensions, dimension);
        if (!descriptor.m_TimeMajor)
        {
            statusConcat = arm_compute::CLConcatenateLayer::validate(concatInputsTensorInfosPtr,
                                                                     &aclConcatOuputTensorInfo,
                                                                     aclAxisConcat);
        }
        else
        {
            statusConcat = arm_compute::CLConcatenateLayer::validate(concatInputsTensorInfosPtr,
                                                                     &aclOutputInfo,
                                                                     aclAxisConcat);
        }
    }
    // If only one LSTM batch, we do not concat and/or permute.
    // Must ensure final output info is expanded to correct batch major dimensions.
    else
    {
        if (!descriptor.m_TimeMajor)
        {
            const_cast<arm_compute::TensorInfo*>(&aclInputInfo)->set_tensor_shape(
                BuildArmComputeTensorShape(shapeExpandBatchMajor));
        }
        else
        {
            const_cast<arm_compute::TensorInfo*>(&aclInputInfo)->set_tensor_shape(
                BuildArmComputeTensorShape(shapeExpandTimeMajor));
        }
    }
    //
    // Permute validate
    //
    if (!descriptor.m_TimeMajor)
    {
        // Output now time major. Permute output back to batch major.
        if (maxTime != 1)
        {
            statusPermute2 = arm_compute::CLPermute::validate(&aclConcatOuputTensorInfo,
                                                              &aclOutputInfo,
                                                              arm_compute::PermutationVector(0U, 2U, 1U));
        }
        else
        {
            statusPermute2 = arm_compute::CLPermute::validate(concatInputsTensorInfosPtr[0],
                                                              &aclOutputInfo,
                                                              arm_compute::PermutationVector(0U, 2U, 1U));
        }
    }

    auto okCode = arm_compute::ErrorCode::OK;
    if (statusPermute1.error_code() == okCode &&
        statusSplit.error_code()    == okCode &&
        statusLSTM .error_code()    == okCode &&
        statusConcat.error_code()   == okCode &&
        statusPermute2.error_code() == okCode)
    {
        return arm_compute::Status(arm_compute::ErrorCode::OK,
                                   "All Unidirectional Sequence LSTM layer validate status OK.");
    }
    else
    {
        return arm_compute::Status(arm_compute::ErrorCode::RUNTIME_ERROR,
                                   "Unidirectional Sequence LSTM layer validate status failed.");
    }
}

void ClUnidirectionalSequenceLstmFloatWorkload::FreeUnusedTensors()
{
    FreeTensorIfUnused(m_InputToInputWeightsTensor);
    FreeTensorIfUnused(m_InputToForgetWeightsTensor);
    FreeTensorIfUnused(m_InputToCellWeightsTensor);
    FreeTensorIfUnused(m_InputToOutputWeightsTensor);
    FreeTensorIfUnused(m_RecurrentToInputWeightsTensor);
    FreeTensorIfUnused(m_RecurrentToForgetWeightsTensor);
    FreeTensorIfUnused(m_RecurrentToCellWeightsTensor);
    FreeTensorIfUnused(m_RecurrentToOutputWeightsTensor);
    FreeTensorIfUnused(m_CellToInputWeightsTensor);
    FreeTensorIfUnused(m_CellToForgetWeightsTensor);
    FreeTensorIfUnused(m_CellToOutputWeightsTensor);
    FreeTensorIfUnused(m_InputGateBiasTensor);
    FreeTensorIfUnused(m_ForgetGateBiasTensor);
    FreeTensorIfUnused(m_CellBiasTensor);
    FreeTensorIfUnused(m_OutputGateBiasTensor);
    FreeTensorIfUnused(m_ProjectionWeightsTensor);
    FreeTensorIfUnused(m_ProjectionBiasTensor);
    FreeTensorIfUnused(m_InputLayerNormWeightsTensor);
    FreeTensorIfUnused(m_ForgetLayerNormWeightsTensor);
    FreeTensorIfUnused(m_CellLayerNormWeightsTensor);
    FreeTensorIfUnused(m_OutputLayerNormWeightsTensor);
    FreeTensorIfUnused(m_ScratchBuffer);
}

} //namespace armnn
