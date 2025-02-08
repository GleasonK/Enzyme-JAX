//===- ArithRaising.cpp - Raise to Arith dialect --------------------------- //
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// This file implements a pass to raise operations to arith dialect.
//===---------------------------------------------------------------------===//

#include "src/enzyme_ad/jax/Passes/Passes.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include <numeric>

namespace mlir {
namespace enzyme {
#define GEN_PASS_DEF_CANONICALIZELOOPSPASS
#include "src/enzyme_ad/jax/Passes/Passes.h.inc"
} // namespace enzyme
} // namespace mlir

using namespace mlir;
using namespace mlir::affine;
using namespace mlir::enzyme;

namespace {

struct RemoveAffineParallelSingleIter
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp op,
                                PatternRewriter &rewriter) const override {
    // Reductions are not supported yet.
    if (!op.getReductions().empty())
      return failure();

    SmallVector<AffineExpr> lbounds(op.getLowerBoundsMap().getResults().begin(),
                                    op.getLowerBoundsMap().getResults().end());
    SmallVector<AffineExpr> ubounds(op.getUpperBoundsMap().getResults().begin(),
                                    op.getUpperBoundsMap().getResults().end());

    SmallVector<int32_t> lboundGroup;
    SmallVector<int32_t> uboundGroup;
    for (auto lb : op.getLowerBoundsGroups())
      lboundGroup.push_back(lb.getZExtValue());
    for (auto ub : op.getUpperBoundsGroups())
      uboundGroup.push_back(ub.getZExtValue());

    SmallVector<int64_t> steps;
    for (auto step : op.getSteps())
      steps.push_back(step);

    Block *tmpBlk = new Block();
    SmallVector<Value> replacements;
    
    bool changed = false;
    for (ssize_t idx = steps.size() - 1; idx >= 0; idx--) {
      replacements.insert(replacements.begin(),
                          tmpBlk->insertArgument((unsigned)0,
                                              op.getIVs()[idx].getType(),
                                              op.getIVs()[idx].getLoc()));
      if (lboundGroup[idx] != 1)
        continue;
      if (uboundGroup[idx] != 1)
        continue;
      size_t loff = 0;
      for (size_t i = 0; i < idx; i++)
        loff += lboundGroup[i];

      size_t uoff = 0;
      for (size_t i = 0; i < idx; i++)
        uoff += uboundGroup[i];

      auto lb = lbounds[loff].dyn_cast<AffineConstantExpr>();
      if (!lb)
        continue;
      auto ub = ubounds[uoff].dyn_cast<AffineConstantExpr>();
      if (!ub)
        continue;
      if (lb.getValue() >= ub.getValue())
        continue;
      if (lb.getValue() + steps[idx] >= ub.getValue()) {
        tmpBlk->eraseArgument(0);
        replacements[0] =
            rewriter.create<arith::ConstantIndexOp>(op.getLoc(), lb.getValue());
        lboundGroup.erase(lboundGroup.begin() + idx);
        uboundGroup.erase(uboundGroup.begin() + idx);
        lbounds.erase(lbounds.begin() + loff);
        ubounds.erase(ubounds.begin() + uoff);
        steps.erase(steps.begin() + idx);
        changed = true;
        continue;
      }
      continue;
    }
    if (!changed) {
      delete tmpBlk;
      return failure();
    }

    if (steps.size() == 0) {
      delete tmpBlk;

      auto yld = cast<affine::AffineYieldOp>(op.getBody()->getTerminator());
      SmallVector<Value> toRet(yld.getOperands());
      rewriter.eraseOp(yld);
      rewriter.inlineBlockBefore(op.getBody(), op, replacements);
      rewriter.replaceOp(op, toRet);
    } else {

      affine::AffineParallelOp affineLoop =
          rewriter.create<affine::AffineParallelOp>(
              op.getLoc(), op.getResultTypes(),
              op.getReductions(),
              AffineMapAttr::get(
                  AffineMap::get(op.getLowerBoundsMap().getNumDims(),
                                 op.getLowerBoundsMap().getNumSymbols(),
                                 lbounds, op.getContext())),
              rewriter.getI32TensorAttr(lboundGroup),
              AffineMapAttr::get(
                  AffineMap::get(op.getUpperBoundsMap().getNumDims(),
                                 op.getUpperBoundsMap().getNumSymbols(),
                                 ubounds, op.getContext())),
              rewriter.getI32TensorAttr(uboundGroup),
              rewriter.getI64ArrayAttr(steps), op.getOperands());

      affineLoop.getRegion().getBlocks().push_back(tmpBlk);

      rewriter.mergeBlocks(op.getBody(), affineLoop.getBody(), replacements);
      rewriter.replaceOp(op, affineLoop->getResults());
    }

    return success();
  }
};

struct CanonicalizeLoopsPass
    : public enzyme::impl::CanonicalizeLoopsPassBase<CanonicalizeLoopsPass> {
  void runOnOperation() override {
    RewritePatternSet patterns(&getContext());

    patterns.add<RemoveAffineParallelSingleIter>(&getContext());

    if (failed(applyPatternsAndFoldGreedily(getOperation(), std::move(patterns)))) {
      signalPassFailure();
    }
  }
};
} // namespace