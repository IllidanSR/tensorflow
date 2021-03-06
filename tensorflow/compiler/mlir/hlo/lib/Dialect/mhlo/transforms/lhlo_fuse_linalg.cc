/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for fusing linalg ops obtained after LHLO
// lowering.

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/Analysis/DependenceAnalysis.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/FoldUtils.h"

namespace mlir {
namespace lmhlo {
namespace {

using linalg::LinalgOp;

class LhloFuseLinalgPass
    : public PassWrapper<LhloFuseLinalgPass, FunctionPass> {
  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<AffineDialect, linalg::LinalgDialect, scf::SCFDialect>();
  }

 public:
  LhloFuseLinalgPass() = default;
  LhloFuseLinalgPass(const LhloFuseLinalgPass&) {}
  LhloFuseLinalgPass(bool use_parallel_loops,
                     llvm::ArrayRef<unsigned> tile_sizes) {
    tile_sizes_ = tile_sizes;
    use_parallel_loops_.setValue(use_parallel_loops);
  }

  void runOnFunction() override {
    auto func = getFunction();

    // TODO(pifon): Remove assumption that the function has a single block.
    if (!llvm::hasSingleElement(func)) {
      emitError(func.getLoc(), "The function needs to have a single block.");
      signalPassFailure();
      return;
    }

    // The fusion in Linalg is currently possible only when the consumer op is
    // tiled. In order to greedily fuse the ops, we have to start from the tiled
    // root linalg ops, i.e. linalg ops that write to output buffers of the
    // function or are returned in case of escaping allocations.
    llvm::SmallDenseSet<Value> result_buffers;
    for (auto func_arg : func.getArguments()) {
      result_buffers.insert(func_arg);
    }
    for (auto& block : func) {
      auto returnOp = mlir::dyn_cast<mlir::ReturnOp>(block.getTerminator());
      if (!returnOp) continue;
      for (auto operand : returnOp.getOperands()) {
        result_buffers.insert(operand);
      }
    }
    // Resolve aliasing operations (like casts) on the result to identify
    // results. This only handles escaping results.
    // TODO(herhut): Use BufferizeAliasAnalysis for this.
    llvm::SmallVector<Value, 4> worklist(result_buffers.begin(),
                                         result_buffers.end());
    while (!worklist.empty()) {
      Value result = worklist.pop_back_val();
      auto definingOp = result.getDefiningOp();
      if (!definingOp) {
        continue;
      }
      if (auto viewLike = dyn_cast<ViewLikeOpInterface>(definingOp)) {
        auto alias = viewLike.getViewSource();
        if (result_buffers.insert(alias).second) {
          worklist.push_back(alias);
        }
      }
    }
    MLIRContext* ctx = func.getContext();
    OpBuilder b(func);
    OperationFolder folder(ctx);
    func.walk([&](linalg::GenericOp generic_op) {
      SmallVector<int64_t, 2> tile_sizes(tile_sizes_.begin(),
                                         tile_sizes_.end());
      if (tile_sizes.empty()) {
        tile_sizes = SmallVector<int64_t, 2>(generic_op.getNumLoops(), 1);
      }
      auto op = cast<LinalgOp>(generic_op.getOperation());
      for (const Value result : op.getOutputBuffers()) {
        if (!result_buffers.count(result)) continue;
        if (tileGenericOp(op, tile_sizes, &b)) {
          generic_op.erase();
          return;
        }
      }
    });
    auto patterns = linalg::getLinalgTilingCanonicalizationPatterns(ctx);
    applyPatternsAndFoldGreedily(func, patterns);

    // Fuse producers of tiled linalg ops.
    llvm::SmallDenseSet<Operation*> erase_set;
    SmallVector<Operation*, 8> linalg_ops;
    func.walk([&](LinalgOp op) { linalg_ops.push_back(op); });
    for (auto* op : llvm::reverse(linalg_ops)) {
      for (unsigned id = 0, e = LinalgOp(op).getNumInputs(); id < e; ++id) {
        linalg::Aliases aliases;
        linalg::LinalgDependenceGraph graph(aliases, linalg_ops);
        if (auto info = fuseProducerOfBuffer(b, op, id, graph, &folder)) {
          auto originalOp = info->originalProducer.getOperation();
          erase_set.insert(originalOp);
          auto originalOpInLinalgOpsVector = std::find_if(
              linalg_ops.begin(), linalg_ops.end(),
              [&](const Operation* op) { return op == originalOp; });
          *originalOpInLinalgOpsVector = info->fusedProducer.getOperation();
        }
      }

      auto patterns = linalg::getLinalgTilingCanonicalizationPatterns(ctx);
      applyPatternsAndFoldGreedily(func, patterns);
    }
    for (auto* e : erase_set) e->erase();
  }

 private:
  bool tileGenericOp(LinalgOp op, ArrayRef<int64_t> tile_sizes, OpBuilder* b) {
    auto loopType = use_parallel_loops_
                        ? linalg::LinalgTilingLoopType::ParallelLoops
                        : linalg::LinalgTilingLoopType::Loops;
    auto tiled_generic_op = linalg::tileLinalgOp(*b, op,
                                                 linalg::LinalgTilingOptions()
                                                     .setTileSizes(tile_sizes)
                                                     .setLoopType(loopType));
    return tiled_generic_op.hasValue();
  }

  Option<bool> use_parallel_loops_{
      *this, "use-parallel-loops",
      llvm::cl::desc(
          "Tiles GenericOp consumer to parallel loops before linalg fusion"),
      llvm::cl::init(false)};

  ListOption<unsigned> tile_sizes_{
      *this, "tile-sizes",
      llvm::cl::desc(
          "Tile sizes by which to tile linalg generic before linalg fusion"),
      llvm::cl::ZeroOrMore, llvm::cl::MiscFlags::CommaSeparated};
};

}  // namespace

std::unique_ptr<FunctionPass> createLhloFuseLinalgPass(
    bool use_parallel_loops, ArrayRef<unsigned> tile_sizes) {
  return std::make_unique<LhloFuseLinalgPass>(use_parallel_loops, tile_sizes);
}

}  // namespace lmhlo
}  // namespace mlir
