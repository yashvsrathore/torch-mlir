//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Also available under a BSD-style license. See LICENSE.
//
//===----------------------------------------------------------------------===//

#include "torch-mlir/Conversion/TorchToMhlo/TorchToMhlo.h"

#include "../PassDetail.h"
#include "./MhloLegalizeUtils.h"
#include "./PopulatePatterns.h"
#include "mhlo/IR/hlo_ops.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "stablehlo/dialect/ChloOps.h"
#include "torch-mlir/Conversion/Utils/Utils.h"
#include "torch-mlir/Dialect/Torch/IR/TorchDialect.h"
#include "torch-mlir/Dialect/Torch/IR/TorchOps.h"
#include "torch-mlir/Dialect/Torch/Utils/Utils.h"
#include "torch-mlir/Dialect/TorchConversion/IR/TorchConversionOps.h"

using namespace mlir;
using namespace mlir::torch;
using namespace mlir::torch::Torch;
using namespace mlir::torch::torch_to_mhlo;

namespace {
Value getBroadcastTensor(PatternRewriter &rewriter, Operation *op, Value tensor,
                         ArrayRef<int64_t> shape, ArrayRef<Value> dimSizes,
                         ArrayRef<int64_t> broadcastDims) {
  auto tensorTy = tensor.getType().dyn_cast<RankedTensorType>();
  auto loc = op->getLoc();
  Value mhloShape = rewriter.create<tensor::FromElementsOp>(loc, dimSizes);

  RankedTensorType outTy =
      RankedTensorType::get(shape, tensorTy.getElementType());

  RankedTensorType attrTy =
      RankedTensorType::get({static_cast<int64_t>(broadcastDims.size())},
                            rewriter.getIntegerType(64));
  auto broadcastAttr = DenseIntElementsAttr::get(attrTy, broadcastDims);

  auto broadcast = rewriter.create<mhlo::DynamicBroadcastInDimOp>(
      loc, outTy, tensor, mhloShape, broadcastAttr);
  return broadcast;
}

Value getPermutedTensor(PatternRewriter &rewriter, Operation *op, Value input,
                        ArrayRef<int64_t> inpTransDims) {
  auto inputTy = input.getType().dyn_cast<RankedTensorType>();
  auto rank = inputTy.getRank();
  auto transDims = mhlo::toPositiveDims(inpTransDims, rank);
  auto inpShape = inputTy.getShape();
  std::vector<int64_t> newShape;
  newShape.reserve(rank);

  for (auto d : transDims) {
    newShape.push_back(inpShape[d]);
  }

  auto attrTy = RankedTensorType::get({static_cast<int64_t>(transDims.size())},
                                      rewriter.getIntegerType(64));
  auto permuteAttr = DenseIntElementsAttr::get(attrTy, transDims);

  auto outTy = RankedTensorType::get(newShape, inputTy.getElementType());
  auto result = rewriter.create<mhlo::TransposeOp>(op->getLoc(), outTy, input,
                                                   permuteAttr);
  return result.getResult();
}

RankedTensorType castContractingDim(PatternRewriter &rewriter, Operation *op,
                                    Value &lhs, Value &rhs, int64_t nBatchDims,
                                    int64_t lhsResultDim, int64_t rhsResultDim,
                                    int64_t lhsContractingDim,
                                    int64_t rhsContractingDim) {
  auto lhsTy = lhs.getType().dyn_cast<RankedTensorType>();
  auto rhsTy = rhs.getType().dyn_cast<RankedTensorType>();

  bool shouldCastLhs = false;
  bool shouldCastRhs = false;
  auto oldLhsShape = lhsTy.getShape();
  auto oldRhsShape = rhsTy.getShape();
  SmallVector<int64_t> lhsShape;
  SmallVector<int64_t> rhsShape;
  SmallVector<int64_t> outShape;

  lhsShape.append(oldLhsShape.begin(), oldLhsShape.end());
  rhsShape.append(oldRhsShape.begin(), oldRhsShape.end());
  // set batch dims
  for (auto k = 0; k < nBatchDims; ++k) {
    if (lhsShape[k] == ShapedType::kDynamicSize && rhsShape[k] >= 0) {
      lhsShape[k] = rhsShape[k];
      shouldCastLhs = true;
    }
    if (rhsShape[k] == ShapedType::kDynamicSize && lhsShape[k] >= 0) {
      rhsShape[k] = lhsShape[k];
      shouldCastRhs = true;
    }
    outShape.push_back(lhsShape[k]);
  }
  // set contracting dims
  auto lhsContractingDimSize = lhsShape[lhsContractingDim];
  auto rhsContractingDimSize = rhsShape[rhsContractingDim];
  if (lhsContractingDimSize != rhsContractingDimSize) {
    if (lhsContractingDimSize == ShapedType::kDynamic &&
        rhsContractingDimSize >= 0) {
      lhsShape[lhsContractingDim] = rhsContractingDimSize;
      shouldCastLhs = true;
    } else if (rhsContractingDimSize == ShapedType::kDynamic &&
               lhsContractingDimSize >= 0) {
      rhsShape[rhsContractingDim] = lhsContractingDimSize;
      shouldCastRhs = true;
    }
  }
  if (shouldCastLhs) {
    auto newRankTy = RankedTensorType::get(lhsShape, lhsTy.getElementType());
    lhs = rewriter.create<tensor::CastOp>(op->getLoc(), newRankTy, lhs);
  }

  if (shouldCastRhs) {
    auto newRankTy = RankedTensorType::get(rhsShape, rhsTy.getElementType());
    rhs = rewriter.create<tensor::CastOp>(op->getLoc(), newRankTy, rhs);
  }

  // set result dimensions
  if (lhsResultDim < static_cast<int64_t>(lhsShape.size()) && lhsResultDim >= 0) {
    outShape.push_back(lhsShape[lhsResultDim]);
  }
  if (rhsResultDim < static_cast<int64_t>(rhsShape.size()) && rhsResultDim >= 0) {
    outShape.push_back(rhsShape[rhsResultDim]);
  }
  return RankedTensorType::get(outShape, lhsTy.getElementType());
}

void getBmmBroadcast(PatternRewriter &rewriter, Operation *op, Value &inpLhs,
                     Value &inpRhs, int64_t leadingRank,
                     size_t dimSizeIndexBits) {
  Value lhs = inpLhs;
  Value rhs = inpRhs;
  auto lhsRankTy = inpLhs.getType().dyn_cast<RankedTensorType>();
  auto rhsRankTy = inpRhs.getType().dyn_cast<RankedTensorType>();

  auto lhsRank = lhsRankTy.getRank();
  auto rhsRank = rhsRankTy.getRank();

  // The non-matrix (i.e. batch) dimensions are broadcasted (and thus must be
  // broadcastable).
  auto minRank = std::min(lhsRank, rhsRank);
  auto leadingDims = llvm::to_vector<4>(llvm::seq<int64_t>(0, leadingRank));
  auto broadcastDims = llvm::to_vector<4>(
      llvm::seq<int64_t>(leadingRank, minRank + leadingRank));
  auto lhsShape = lhsRankTy.getShape();
  auto rhsShape = rhsRankTy.getShape();
  if (lhsRank < rhsRank) {
    std::vector<int64_t> newShape(rhsShape.begin(),
                                  rhsShape.begin() + leadingRank);
    newShape.insert(newShape.end(), lhsShape.begin(), lhsShape.end());
    auto newDimSizes = *mhlo::getDimSizesOfTensor(
        rewriter, op, rhs, leadingDims, dimSizeIndexBits);
    auto lhsDimSizes =
        *mhlo::getDimSizesOfTensor(rewriter, op, lhs, dimSizeIndexBits);
    newDimSizes.insert(newDimSizes.end(), lhsDimSizes.begin(),
                       lhsDimSizes.end());
    lhs = getBroadcastTensor(rewriter, op, lhs, newShape, newDimSizes,
                             broadcastDims);
  } else {
    std::vector<int64_t> newShape(lhsShape.begin(),
                                  lhsShape.begin() + leadingRank);
    newShape.insert(newShape.end(), rhsShape.begin(), rhsShape.end());
    auto newDimSizes = *mhlo::getDimSizesOfTensor(
        rewriter, op, lhs, leadingDims, dimSizeIndexBits);
    auto rhsDimSizes =
        *mhlo::getDimSizesOfTensor(rewriter, op, rhs, dimSizeIndexBits);
    newDimSizes.insert(newDimSizes.end(), rhsDimSizes.begin(),
                       rhsDimSizes.end());
    rhs = getBroadcastTensor(rewriter, op, rhs, newShape, newDimSizes,
                             broadcastDims);
  }

  inpLhs = lhs;
  inpRhs = rhs;
}

// Perform the basic n-dim matmul operation encompassing the handling of
// broadcasting and dynamic shape propagation.
// All PyTorch ops that leverage matrix multiplication will derive this and
// implement their specialized input processing (e.g transpose), and output
// processing, e.g. GEMM or fully connected bias handling.
template <typename AtenOpT>
class ConvertAtenMatmulBaseOp : public ConvertAtenOp<AtenOpT> {
public:
  using ConvertAtenOp<AtenOpT>::ConvertAtenOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  // Each variant must implement corresponding parameter parsing options.
  // Maintain separate input read functions for each variant because it is not
  // necessarily true with all variants that the first two operands are the lhs
  // and rhs.
  virtual LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                         ConversionPatternRewriter &rewriter,
                                         Value &lhs, Value &rhs) const {
    return rewriter.notifyMatchFailure(
        op,
        "unimplemented matrix multiplication variant input parsing function");
  }
  LogicalResult performMatmul(AtenOpT op, OpAdaptor adaptor,
                              ConversionPatternRewriter &rewriter, Value &lhs,
                              Value &rhs, Value &output) const {
    auto lhsTy = lhs.getType().cast<RankedTensorType>();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();
    auto lhsElemTy = lhsTy.getElementType();
    auto rhsElemTy = rhsTy.getElementType();

    if (lhsElemTy != rhsElemTy)
      return op.emitError("matmul: input datatypes mismatched");
    if (lhsRank < 1 || rhsRank < 1) {
      return op.emitError("matmul: inputs can't be 0-rank");
    }

    const auto &options = ConvertAtenOp<AtenOpT>::getOptions();
    auto loc = op->getLoc();

    if (lhsRank <= 2 && rhsRank <= 2) {
      output = rewriter.create<mhlo::DotOp>(loc, lhs, rhs, nullptr);
      return success();
    }

    // for lhsRank > 2 and rhsRrank == 2
    // use dynamicReshapeOp to convert lhs to 2-D Tensor,
    // then use Dot op to perform matrix * vector/matrix,
    // and finnally reshape to right shape,
    // to avoid extra data copy and compute.

    if (rhsRank == 2) {
      SmallVector<Value> resultDims;
      auto dotLhsTy = RankedTensorType::get(
          {ShapedType::kDynamicSize, lhsTy.getShape()[lhsRank - 1]}, lhsElemTy);
      Type intType = rewriter.getIntegerType(options.dimSizeIndexBits);
      Value numel = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getIntegerAttr(intType, 1));
      // reshape lhs to 2-D tensor and record output shape
      for (int i = 0; i < lhsRank - 1; ++i) {
        Value dimValue = rewriter.create<tensor::DimOp>(loc, lhs, i);
        resultDims.push_back(dimValue);
        numel = rewriter.create<arith::MulIOp>(
            loc, numel,
            rewriter.create<arith::IndexCastOp>(loc, intType, dimValue));
      }
      Value lhsLastRankDim = rewriter.create<arith::IndexCastOp>(
          loc, intType, rewriter.create<tensor::DimOp>(loc, lhs, lhsRank - 1));
      if (rhsRank == 2) {
        resultDims.push_back(rewriter.create<tensor::DimOp>(loc, rhs, 1));
      }
      Value reshapeDim =
          rewriter
              .create<mlir::tensor::FromElementsOp>(
                  op->getLoc(), ValueRange{numel, lhsLastRankDim})
              .getResult();
      lhs = rewriter.create<mhlo::DynamicReshapeOp>(loc, dotLhsTy, lhs,
                                                    reshapeDim);
      Value matmulOutput = rewriter.create<mhlo::DotOp>(loc, lhs, rhs, nullptr);
      auto outTy =
          ConvertAtenOp<AtenOpT>::getTypeConverter()->convertType(op.getType());
      // reshape result to output shape
      output = rewriter.create<mhlo::DynamicReshapeOp>(
          loc, outTy, matmulOutput,
          rewriter.create<mlir::tensor::FromElementsOp>(loc, resultDims));
      return success();
    }

    int64_t nBatchDims;
    if (rhsRank == 1) {
      auto leadingRank = lhsRank - 2;
      getBmmBroadcast(rewriter, op, lhs, rhs, leadingRank,
                      options.dimSizeIndexBits);
      nBatchDims = leadingRank;
    } else if (lhsRank <= 2) {
      auto leadingRank = rhsRank - 2;
      getBmmBroadcast(rewriter, op, lhs, rhs, leadingRank,
                      options.dimSizeIndexBits);
      nBatchDims = leadingRank;
    } else {
      assert(rhsRank > 2 && lhsRank > 2);
      auto leadingRank = std::max(lhsRank - rhsRank, rhsRank - lhsRank);
      nBatchDims = std::max(lhsRank - 2, rhsRank - 2);
      getBmmBroadcast(rewriter, op, lhs, rhs, leadingRank,
                      options.dimSizeIndexBits);
    }
    auto batchDims = llvm::to_vector<4>(llvm::seq<int64_t>(0, nBatchDims));

    auto lhsResultDim = nBatchDims;
    auto rhsResultDim = nBatchDims + 1;
    auto lhsContractingDim = nBatchDims + 1;
    auto rhsContractingDim = nBatchDims;
    if (lhsRank == 1) {
      lhsResultDim = nBatchDims + 1;
      lhsContractingDim = nBatchDims;
    }

    mhlo::DotDimensionNumbersAttr dotDimensionNumbers =
        mhlo::DotDimensionNumbersAttr::get(
            rewriter.getContext(),
            /*lhsBatchingDimensions=*/batchDims,
            /*rhsBatchingDimensions=*/batchDims,
            /*lhsContractingDimensions=*/{lhsContractingDim},
            /*rhsContractingDimensions=*/{rhsContractingDim});
    auto outTy =
        castContractingDim(rewriter, op, lhs, rhs, nBatchDims, lhsResultDim,
                           rhsResultDim, lhsContractingDim, rhsContractingDim);
    output = rewriter
                 .create<mhlo::DotGeneralOp>(loc, outTy, lhs, rhs,
                                             dotDimensionNumbers, nullptr)
                 .getResult();
    return success();
  }

  // The default version just reads two inputs, computes output and returns it.
  // Other versions may add a bias, apply GEMM-style alpha/beta scaling etc.
  virtual LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs, rhs;
    if (failed(readMatMulInputs(op, adaptor, rewriter, lhs, rhs)))
      return op.emitError("failed to read matmul inputs");

    Value output;

    if (failed(performMatmul(op, adaptor, rewriter, lhs, rhs, output)))
      return op.emitError("failed to perform matmul operation");

    rewriter.replaceOpWithNewOp<tensor::CastOp>(
        op,
        ConvertAtenOp<AtenOpT>::getTypeConverter()
            ->convertType(op.getType())
            .template cast<RankedTensorType>(),
        output);

    return success();
  }
};

// Legalizes the torch.matmul op for general n-dim matmul.
template <typename AtenOpT>
class ConvertAtenMatMulOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {
    lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getOther();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return op.emitError(
          "only ranked tensor types are supported in MHLO matmul");

    return success();
  }
};

// Implements handling of aten.mm and aten.bmm ops.
template <typename AtenOpT>
class ConvertAtenMmOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {
    lhs = adaptor.getSelf();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getMat2();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return op.emitError(
          "only ranked tensor types are supported in MHLO matmul");

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();

    if (isa<AtenMmOp>(op)) {
      // Mm takes two 2D tensors.
      if (lhsRank != 2 || rhsRank != 2)
        return op.emitError("aten.mm called but matrix rank != 2");
    } else if (isa<AtenBmmOp>(op)) {
      // Bmm takes two 3D tensors.
      if (lhsRank != 3 || rhsRank != 3)
        return op.emitError("aten.bmm called but matrix rank != 3");
    }

    return success();
  }
};

// Implements handling of aten.linear op.
template <typename AtenOpT>
class ConvertAtenLinearOp : public ConvertAtenMatmulBaseOp<AtenOpT> {
public:
  using ConvertAtenMatmulBaseOp<AtenOpT>::ConvertAtenMatmulBaseOp;
  using OpAdaptor = typename AtenOpT::Adaptor;
  LogicalResult readMatMulInputs(AtenOpT op, OpAdaptor adaptor,
                                 ConversionPatternRewriter &rewriter,
                                 Value &lhs, Value &rhs) const override {
    lhs = adaptor.getInput();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();

    rhs = adaptor.getWeight();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();

    if (!lhsTy || !rhsTy)
      return op.emitError(
          "only ranked tensor types are supported in MHLO matmul");

    auto lhsRank = lhsTy.getRank();
    auto rhsRank = rhsTy.getRank();

    if (lhsRank < 1)
      return op.emitError("aten.Linear called but input rank 0");
    if (rhsRank != 2)
      return op.emitError("aten.Linear called but weight rank not 2");

    return success();
  }
  // Override the default rewriter to perform RHS transpose and bias addition
  // as well.
  LogicalResult
  matchAndRewrite(AtenOpT op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value lhs, rhs;

    if (failed(readMatMulInputs(op, adaptor, rewriter, lhs, rhs)))
      return op.emitError("failed to read matmul inputs");

    // The aten.Linear op has a bias tensor that is added to the matmul
    // output.
    auto bias = adaptor.getBias();
    auto biasTy = bias.getType();

    // MHLO does not mandate that elementwise op tensors need to be ranked.
    if (!biasTy.template isa<Torch::NoneType>() &&
        !biasTy.template isa<RankedTensorType>())
      return op.emitError("only ranked tensor types are supported in MHLO "
                          "matmul for bias tensor");

    // weight.T
    rhs = getPermutedTensor(rewriter, op, rhs, {1, 0});

    auto lhsTy = lhs.getType().cast<RankedTensorType>();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();
    auto lhsRank = lhsTy.getRank();

    auto loc = op->getLoc();
    Value dotLhs;
    SmallVector<Value> resultDims;
    // vector * matrix or matrix * matrix can directly use mhlo.dot_general
    if (lhsTy.getRank() <= 2) {
      dotLhs = lhs;
    } else {
      // Broadcast weight and then use bmm would lead to too much data copy,
      // then decreace the performance
      // Instead, reshape input to 2-D tensor, then use dot to perform
      // matrix-matrix multiply, and finnaly reshape to the output shape,
      // would get better performance

      // [x_1, x_2, ..., x_n, in_features] * [in_features, out_features]
      // -> [x_1 * x_2 * ... * x_n , in_features] * [in_features, out_features]
      auto dotLhsTy = RankedTensorType::get(
          {ShapedType::kDynamicSize, lhsTy.getShape()[lhsRank - 1]},
          lhsTy.getElementType());
      const auto &options = ConvertAtenOp<AtenOpT>::getOptions();
      Type intType = rewriter.getIntegerType(options.dimSizeIndexBits);
      Value numel = rewriter.create<arith::ConstantOp>(
          loc, rewriter.getIntegerAttr(intType, 1));

      for (int i = 0; i < lhsRank - 1; ++i) {
        Value dimValue = rewriter.create<tensor::DimOp>(loc, lhs, i);
        resultDims.push_back(dimValue);
        numel = rewriter.create<arith::MulIOp>(
            loc, numel,
            rewriter.create<arith::IndexCastOp>(loc, intType, dimValue));
      }
      Value lhsLastRankDim = rewriter.create<arith::IndexCastOp>(
          loc, intType, rewriter.create<tensor::DimOp>(loc, lhs, lhsRank - 1));
      resultDims.push_back(rewriter.create<tensor::DimOp>(loc, rhs, 1));
      Value reshapeDim =
          rewriter
              .create<mlir::tensor::FromElementsOp>(
                  op->getLoc(), ValueRange{numel, lhsLastRankDim})
              .getResult();
      dotLhs = rewriter.create<mhlo::DynamicReshapeOp>(loc, dotLhsTy, lhs,
                                                       reshapeDim);
    }
    Value matmulOutput =
        rewriter.create<mhlo::DotOp>(loc, dotLhs, rhs, nullptr);
    auto outTy =
        ConvertAtenOp<AtenOpT>::getTypeConverter()->convertType(op.getType());
    // reshape to [x_1, x_2, ..., x_n, out_features]
    if (dotLhs != lhs) {
      matmulOutput = rewriter.create<mhlo::DynamicReshapeOp>(
          loc, outTy, matmulOutput,
          rewriter.create<mlir::tensor::FromElementsOp>(loc, resultDims));
    }

    Value matmulPlusBias = matmulOutput;
    if (!biasTy.template isa<Torch::NoneType>()) {
      // Bias addition broadcasts to the matmul output shape.
      matmulPlusBias = rewriter
                           .create<chlo::BroadcastAddOp>(
                               op->getLoc(), outTy, matmulOutput, bias, nullptr)
                           .getResult();
    }

    rewriter.replaceOp(op, matmulPlusBias);
    return success();
  }
};

class ConvertAtenConvolutionOp : public ConvertAtenOp<AtenConvolutionOp> {
public:
  using ConvertAtenOp<AtenConvolutionOp>::ConvertAtenOp;
  using OpAdaptor = typename AtenConvolutionOp::Adaptor;

  Value reshapeConvWeight(PatternRewriter &rewriter, Operation *op,
                          Value weight, int64_t groups) const {
    auto weightTy = weight.getType().cast<RankedTensorType>();
    auto weightElemTy = weightTy.getElementType();
    auto rank = weightTy.getRank();
    const auto &options = getOptions();
    SmallVector<Value> weightShapeVec = *mhlo::getDimSizesOfTensor(
        rewriter, op, weight, options.dimSizeIndexBits);
    auto weightShape = weightTy.getShape();
    SmallVector<int64_t> weightShapeInt(rank);
    std::copy(weightShape.begin(), weightShape.end(), weightShapeInt.begin());

    // 1. [IC, OC, H, W, ...] => [G, IC//G, OC, H, W, ...]
    Value GValue = rewriter.create<mlir::arith::ConstantOp>(
        op->getLoc(), rewriter.getI64IntegerAttr(groups));
    Value ICDivGValue = rewriter.create<mlir::arith::DivSIOp>(
        op->getLoc(), weightShapeVec[0], GValue);
    Value OCMulGValue = rewriter.create<mlir::arith::MulIOp>(
        op->getLoc(), weightShapeVec[1], GValue);
    weightShapeVec[0] = ICDivGValue;
    weightShapeVec.insert(weightShapeVec.begin(), GValue);

    if (weightShapeInt[0] == ShapedType::kDynamic) {
      weightShapeInt.insert(weightShapeInt.begin(), groups);
    } else {
      weightShapeInt[0] /= groups;
      weightShapeInt.insert(weightShapeInt.begin(), groups);
    }
    Value weightShapeTensor = rewriter.create<mlir::tensor::FromElementsOp>(
        op->getLoc(), weightShapeVec);
    weight = rewriter.create<mhlo::DynamicReshapeOp>(
        op->getLoc(), RankedTensorType::get(weightShapeInt, weightElemTy),
        weight, weightShapeTensor);

    // 2. [G, IC//G, OC, H, W, ...] => [IC//G, G, OC, H, W, ...]
    std::vector<int64_t> transposeDims(rank + 1);
    for (int64_t i = 0; i <= rank; i++)
      transposeDims[i] = i;
    std::swap(transposeDims[1], transposeDims[0]);
    weight = rewriter.create<mhlo::TransposeOp>(
        op->getLoc(), weight, rewriter.getI64TensorAttr(transposeDims));

    // 3. [IC//G, G, OC, H, W, ...] => [IC//G, G*OC, H, W, ...]
    weightShapeInt.erase(weightShapeInt.begin());
    if (weightShapeInt[1] != ShapedType::kDynamic) {
      weightShapeInt[1] *= groups;
    }
    weightShapeVec.erase(weightShapeVec.begin());
    weightShapeVec[1] = OCMulGValue;
    weightShapeTensor = rewriter.create<mlir::tensor::FromElementsOp>(
        op->getLoc(), weightShapeVec);
    weight = rewriter.create<mhlo::DynamicReshapeOp>(
        op->getLoc(), RankedTensorType::get(weightShapeInt, weightElemTy),
        weight, weightShapeTensor);
    return weight;
  }

  Value convertTransposedConv(AtenConvolutionOp op,
                              ConversionPatternRewriter &rewriter,
                              RankedTensorType outType, Value input,
                              Value weight, ArrayRef<int64_t> stride,
                              ArrayRef<int64_t> padding,
                              ArrayRef<int64_t> dilation,
                              ArrayRef<int64_t> outputPadding, int64_t groups,
                              bool needHandleOutputPadding) const {
    auto inputTy = input.getType().cast<RankedTensorType>();
    auto weightTy = weight.getType().cast<RankedTensorType>();
    auto weightShape = weightTy.getShape();

    auto nDims = inputTy.getRank();
    auto nSpatialDims = nDims - 2;
    auto convOutTy = outType;

    if (needHandleOutputPadding) {
      SmallVector<int64_t> outShape(nDims);
      auto finalOutShape = outType.getShape();
      std::copy(finalOutShape.begin(), finalOutShape.end(), outShape.begin());
      for (int i = 2; i < nDims; ++i) {
        if (finalOutShape[i] == ShapedType::kDynamic)
          continue;
        outShape[i] = finalOutShape[i] - outputPadding[i - 2];
      }
      convOutTy = RankedTensorType::get(outShape, outType.getElementType());
    }

    // Prepare for transposed convolution
    SmallVector<int64_t> mhloStrideVec(nSpatialDims, 1);
    DenseIntElementsAttr mhloStride = rewriter.getI64TensorAttr(mhloStrideVec);
    SmallVector<int64_t> mhloPaddingVec(nSpatialDims * 2, 0);
    for (int i = 0; i < nSpatialDims; ++i) {
      int64_t padInt = dilation[i] * (weightShape[i + 2] - 1) - padding[i];
      mhloPaddingVec[i * 2] = padInt;
      mhloPaddingVec[i * 2 + 1] = padInt;
    }
    DenseIntElementsAttr mhloPadding = DenseIntElementsAttr::get(
        RankedTensorType::get({nSpatialDims, 2}, rewriter.getI64Type()),
        mhloPaddingVec);
    SmallVector<int64_t> mhloLhsDilationVec(nSpatialDims);
    std::copy(stride.begin(), stride.end(), mhloLhsDilationVec.begin());
    DenseIntElementsAttr mhloLhsDilation =
        rewriter.getI64TensorAttr(mhloLhsDilationVec);
    SmallVector<int64_t> mhloRhsDilationVec(nSpatialDims);
    std::copy(dilation.begin(), dilation.end(), mhloRhsDilationVec.begin());
    DenseIntElementsAttr mhloRhsDilation =
        rewriter.getI64TensorAttr(mhloRhsDilationVec);

    DenseElementsAttr windowReversal;
    ArrayAttr precisionConfig;

    SmallVector<int64_t> spatialDims;
    for (int i = 0; i < nSpatialDims; ++i) {
      spatialDims.push_back(i + 2);
    }
    mhlo::ConvDimensionNumbersAttr dimensionNumbers =
        mhlo::ConvDimensionNumbersAttr::get(
            /*context=*/rewriter.getContext(), /*inputBatchDimension=*/0,
            /*inputFeatureDimension=*/1,
            /*inputSpatialDimensions=*/spatialDims,
            /*kernelInputFeatureDimension=*/0,
            /*kernelOutputFeatureDimension=*/1,
            /*kernelSpatialDimensions=*/spatialDims,
            /*outputBatchDimension=*/0, /*outputFeatureDimension=*/1,
            /*outputSpatialDimensions=*/spatialDims);

    // Reverse and transpose weight
    weight = rewriter.create<mhlo::ReverseOp>(
        op->getLoc(), weight, rewriter.getI64TensorAttr(spatialDims));
    if (groups != 1) {
      weight = reshapeConvWeight(rewriter, op, weight, groups);
    }

    // Create transposed convolution
    auto transposedConvOp = rewriter.create<mhlo::ConvolutionOp>(
        op->getLoc(), convOutTy, input, weight, mhloStride, mhloPadding,
        mhloLhsDilation, mhloRhsDilation, windowReversal, dimensionNumbers,
        static_cast<uint64_t>(groups), 1, precisionConfig);

    // Handle output padding
    if (!needHandleOutputPadding) {
      return transposedConvOp.getResult();
    }
    SmallVector<int64_t> edgePaddingLowVec(nDims, 0);
    SmallVector<int64_t> edgePaddingHighVec(nDims, 0);
    SmallVector<int64_t> interiorPaddingVec(nDims, 0);
    std::copy(outputPadding.begin(), outputPadding.end(),
              edgePaddingHighVec.begin() + 2);
    Value paddingValue =
        mhlo::getConstTensor<float>(rewriter, op, {0.0}, {}).value();
    paddingValue = mhlo::promoteType(rewriter, paddingValue, inputTy);
    mlir::DenseIntElementsAttr edgePaddingLow =
        rewriter.getI64VectorAttr(edgePaddingLowVec);
    mlir::DenseIntElementsAttr edgePaddingHigh =
        rewriter.getI64VectorAttr(edgePaddingHighVec);
    mlir::DenseIntElementsAttr interiorPadding =
        rewriter.getI64VectorAttr(interiorPaddingVec);

    auto paddedOutput = rewriter.create<mhlo::PadOp>(
        op->getLoc(), outType, transposedConvOp, paddingValue, edgePaddingLow,
        edgePaddingHigh, interiorPadding);

    return paddedOutput.getResult();
  }

  Value convertNormalConv(AtenConvolutionOp op,
                          ConversionPatternRewriter &rewriter,
                          RankedTensorType outType, Value input, Value weight,
                          ArrayRef<int64_t> stride, ArrayRef<int64_t> padding,
                          ArrayRef<int64_t> dilation, int64_t groups) const {
    int64_t nDims = outType.getRank();

    // Get mhlo::ConvolutionOp attributes
    DenseIntElementsAttr mhloWindowStride = DenseIntElementsAttr::get(
        RankedTensorType::get({static_cast<long int>(stride.size())},
                              rewriter.getI64Type()),
        stride);
    std::vector<int64_t> mhloPaddingVec;
    for (size_t i = 0; i < padding.size(); i++) {
      mhloPaddingVec.emplace_back(padding[i]);
      mhloPaddingVec.emplace_back(padding[i]);
    }
    DenseIntElementsAttr mhloPadding = DenseIntElementsAttr::get(
        RankedTensorType::get(
            {static_cast<long int>(padding.size()), static_cast<long int>(2)},
            rewriter.getI64Type()),
        mhloPaddingVec);
    DenseIntElementsAttr mhloRhsDilation = DenseIntElementsAttr::get(
        RankedTensorType::get({static_cast<long int>(dilation.size())},
                              rewriter.getI64Type()),
        dilation);
    SmallVector<int64_t> spatialDimensions;
    for (int64_t i = 2; i < nDims; i++) {
      spatialDimensions.emplace_back(i);
    }
    mhlo::ConvDimensionNumbersAttr dimensionNumbers =
        mhlo::ConvDimensionNumbersAttr::get(
            /*context=*/rewriter.getContext(), /*inputBatchDimension=*/0,
            /*inputFeatureDimension=*/1,
            /*inputSpatialDimensions=*/spatialDimensions,
            /*kernelInputFeatureDimension=*/1,
            /*kernelOutputFeatureDimension=*/0,
            /*kernelSpatialDimensions=*/spatialDimensions,
            /*outputBatchDimension=*/0, /*outputFeatureDimension=*/1,
            /*outputSpatialDimensions=*/spatialDimensions);

    // mhlo::ConvolutionOp's optional attributes, leave them as default
    DenseIntElementsAttr mhloLhsDilation;
    DenseElementsAttr windowReversal;
    ArrayAttr precisionConfig;

    auto mhloConvOp = rewriter.create<mhlo::ConvolutionOp>(
        op->getLoc(), outType, input, weight, mhloWindowStride, mhloPadding,
        mhloLhsDilation, mhloRhsDilation, windowReversal, dimensionNumbers,
        static_cast<uint64_t>(groups), 1, precisionConfig);

    return mhloConvOp.getResult();
  }

  LogicalResult
  matchAndRewrite(AtenConvolutionOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value input = adaptor.getInput();
    Value weight = adaptor.getWeight();

    // The input shape is [N, C, H, W]
    auto inputTy = input.getType().template cast<RankedTensorType>();
    // The weight shape is [OC, (IC//G), KH, KW]
    // If transposed is set to true,
    // the weight shape changes to [IC, (OC//G), KH, KW]
    auto weightTy = weight.getType().template cast<RankedTensorType>();
    auto outTy = getTypeConverter()
                     ->convertType(op.getType())
                     .template cast<RankedTensorType>();
    if (!inputTy || !weightTy || !outTy) {
      return op.emitError("input, weight and output must be ranked tensors");
    }
    if (inputTy.getRank() < 3)
      return op.emitError("only input with at least 3 dims valid");
    SmallVector<int64_t> stride;
    if (!matchPattern(op.getStride(), m_TorchListOfConstantInts(stride))) {
      return rewriter.notifyMatchFailure(op,
                                         "non-const stride list unsupported");
    }
    SmallVector<int64_t> padding;
    if (!matchPattern(op.getPadding(), m_TorchListOfConstantInts(padding))) {
      return rewriter.notifyMatchFailure(op,
                                         "non-const padding list unsupported");
    }
    SmallVector<int64_t> dilation;
    if (!matchPattern(op.getDilation(), m_TorchListOfConstantInts(dilation))) {
      return rewriter.notifyMatchFailure(op,
                                         "non-const dilation list unsupported");
    }
    SmallVector<int64_t> outputPadding;
    if (!matchPattern(op.getOutputPadding(),
                      m_TorchListOfConstantInts(outputPadding))) {
      return rewriter.notifyMatchFailure(
          op, "non-const output_padding list unsupported");
    }
    int64_t groups;
    if (!matchPattern(op.getGroups(), m_TorchConstantInt(&groups))) {
      return rewriter.notifyMatchFailure(op, "non-int groups unsupported");
    }
    bool transposed;
    if (!matchPattern(op.getTransposed(), m_TorchConstantBool(&transposed))) {
      return rewriter.notifyMatchFailure(op, "non-bool transposed unsupported");
    }
    // Whether need to handle outputpadding
    bool needHandleOutputPadding = false;
    for (int64_t i : outputPadding) {
      if (i != 0) {
        needHandleOutputPadding = true;
        break;
      }
    }
    // Op validation check
    if (needHandleOutputPadding && !transposed) {
      return op->emitError(
          "output padding attr is valid only in transposed convolution");
    }
    assert(padding.size() == dilation.size() &&
           padding.size() == stride.size() &&
           padding.size() == static_cast<size_t>(inputTy.getRank()) - 2 &&
           inputTy.getRank() == weightTy.getRank());

    auto nSpatialDims = padding.size();
    auto nDims = inputTy.getRank();

    // Kernel size must be constant.
    auto weightShape = weightTy.getShape();
    for (int i = 2; i < nDims; ++i) {
      if (weightShape[i] == ShapedType::kDynamic) {
        return rewriter.notifyMatchFailure(
            op, "only constant kernel size is supported");
      }
    }

    Value mhloConvResult;
    if (transposed) {
      mhloConvResult = convertTransposedConv(
          op, rewriter, outTy, input, weight, stride, padding, dilation,
          outputPadding, groups, needHandleOutputPadding);
    } else {
      mhloConvResult = convertNormalConv(op, rewriter, outTy, input, weight,
                                         stride, padding, dilation, groups);
    }

    auto bias = adaptor.getBias();

    // No bias provided
    if (failed(checkNotNone(rewriter, op, op.getBias()))) {
      rewriter.replaceOp(op, mhloConvResult);
      return success();
    }

    // Handle bias
    if (!bias.getType().cast<RankedTensorType>()) {
      return op.emitError("bias provided but not a ranked tensor");
    }

    auto biasTy = bias.getType().cast<RankedTensorType>();
    if (!biasTy.getElementType().isIntOrFloat()) {
      return op.emitError("only floating-point or integer datatype "
                          "legalization for bias supported");
    }

    assert(biasTy.getRank() <= 1);

    // Reshape and promote bias
    auto inputUnsqzDims =
        llvm::to_vector<4>(llvm::seq<int64_t>(-nSpatialDims, 0));

    const auto &options = getOptions();
    bias = *mhlo::unsqueezeTensor(rewriter, op, bias, inputUnsqzDims,
                                  options.dimSizeIndexBits);
    bias = mhlo::promoteType(rewriter, bias, outTy);

    DenseIntElementsAttr bcastDimensions;
    rewriter.replaceOpWithNewOp<chlo::BroadcastAddOp>(op, outTy, mhloConvResult,
                                                      bias, bcastDimensions);
    return success();
  }
};
} // namespace

void mlir::torch::torch_to_mhlo::populateLinearOpPatternsAndLegality(
    TypeConverter &typeConverter, RewritePatternSet &patterns,
    ConversionTarget &target, const TorchToMhloOptions &options) {
  MLIRContext *context = patterns.getContext();

#define INSERT_MATMUL_ATENOP_PATTERN(AtenOp)                                   \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMatMulOp<AtenOp>>(typeConverter, context, options)
  INSERT_MATMUL_ATENOP_PATTERN(AtenMatmulOp);
#undef INSERT_MATMUL_ATEMOP_PATTERN

#define INSERT_MM_ATENOP_PATTERN(AtenOp)                                       \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenMmOp<AtenOp>>(typeConverter, context, options)
  INSERT_MM_ATENOP_PATTERN(AtenMmOp);
  INSERT_MM_ATENOP_PATTERN(AtenBmmOp);
#undef INSERT_MM_ATEMOP_PATTERN

#define INSERT_LINEAR_ATENOP_PATTERN(AtenOp)                                   \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenLinearOp<AtenOp>>(typeConverter, context, options)
  INSERT_LINEAR_ATENOP_PATTERN(AtenLinearOp);
#undef INSERT_LINEAR_ATEMOP_PATTERN

#define INSERT_CONVOLUTION_ATENOP_PATTERN(AtenOp)                              \
  target.addIllegalOp<AtenOp>();                                               \
  patterns.add<ConvertAtenConvolutionOp>(typeConverter, context, options)
  INSERT_CONVOLUTION_ATENOP_PATTERN(AtenConvolutionOp);
#undef INSERT_CONVOLUTION_ATENOP_PATTERN
}