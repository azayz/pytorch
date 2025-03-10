#include <ATen/core/dispatch/OperatorOptions.h>
#include <c10/core/ScalarType.h>
#include <gtest/gtest.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/ir/irparser.h>
#include <torch/csrc/jit/runtime/static/ProcessedNodeInputs.h>
#include <torch/csrc/jit/runtime/static/impl.h>
#include <stdexcept>

#include "deep_wide_pt.h"
#include "test_utils.h"

using namespace caffe2;
using namespace torch;
using namespace torch::jit;
using namespace torch::jit::test;
using c10::IValue;

/*
 When adding a test for an operator implemented in static runtime, there are
 several things that you need to pay attention to:

 1) if the op is an out variant, in the test script of the op,
 instead of:
    def forward(self, input):
      return myop(input)

  do:
    def forward(self, input):
      return myop(input).clone()

 This makes sure that the output of myop is managed by the memory planner and
 exercise the code path in the op impl that otherwise doesn't get exercised. The
 output of the model is not managed by the memory planner, because it needs to
 be returned to the client.

 2) The memory planner rounds up the size of each Tensor's storage to multiples
 of 64 bytes (alignment requirement on AVX512). Make sure the sizes of the input
 tensors in args2 are big enough to trigger resizing.

 3) for view ops such as aten::reshape or aten::to, if you want it to be
 replaced by the copy version with the ReplaceWithCopy pass in passes.h, you
 also want to make sure its output is not returned as the model output. The
 reason is that ReplaceWithCopy only replaces the op whose output is not an
 alias of the model output.
*/

C10_DECLARE_bool(static_runtime_enable_fast_math);

TEST(StaticRuntime, UnaryOps) {
  const auto aten_sum = R"JIT(
    def forward(self, input):
        return torch.sum(input).clone()
  )JIT";

  const auto aten_sum_0 = R"JIT(
    def forward(self, input):
        return torch.sum(input, 0).clone()
  )JIT";

  const auto aten_sum_1 = R"JIT(
    def forward(self, input):
        return torch.sum(input, 1).clone()
  )JIT";

  const auto aten_sum_0_true = R"JIT(
    def forward(self, input):
        return torch.sum(input, 0, True).clone()
  )JIT";

  const auto aten_sum_1_true = R"JIT(
    def forward(self, input):
        return torch.sum(input, 1, True).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({3, 3, 6});

  std::vector<IValue> args{a}, args2{b};

  // sum
  testStaticRuntime(aten_sum, args);
  testStaticRuntime(aten_sum_0, args);
  testStaticRuntime(aten_sum_1, args);
  testStaticRuntime(aten_sum_0_true, args);
  testStaticRuntime(aten_sum_1_true, args);

  testStaticRuntime(aten_sum, args, args2, false, false, false);
  testStaticRuntime(aten_sum_0, args, args2);
  testStaticRuntime(aten_sum_1, args, args2);
  testStaticRuntime(aten_sum_0_true, args, args2);
  testStaticRuntime(aten_sum_1_true, args, args2);
}

TEST(StaticRuntime, Sigmoid) {
  const auto sigmoid_script = R"JIT(
    def forward(self, inp: Tensor):
        b = torch.sigmoid(inp).clone()
        return (b)
  )JIT";
  auto a = at::randn({2, 3});
  auto b = at::randn({4, 3, 2});

  std::vector<IValue> args{a}, args2{b};

  testStaticRuntime(sigmoid_script, args, /*args2=*/{}, /*use_allclose=*/true);
  testStaticRuntime(sigmoid_script, args, {args2}, /*use_allclose=*/true);

  FLAGS_static_runtime_enable_fast_math = false;
  testStaticRuntime(sigmoid_script, args, /*args2=*/{}, /*use_allclose=*/true);
  testStaticRuntime(sigmoid_script, args, {args2}, /*use_allclose=*/true);
  FLAGS_static_runtime_enable_fast_math = true;
}

TEST(StaticRuntime, Clone) {
  const auto clone_script_0 = R"JIT(
    def forward(self, input):
        a = torch.clone(input)
        return (a * a)
  )JIT";

  const auto clone_script_1 = R"JIT(
    def forward(self, input: Tensor, memory_format: int):
        a = torch.clone(input, memory_format=memory_format)
        return (a * a)
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({3, 2}).as_strided({3, 2}, {1, 3});
  auto c = at::randn({1, 2, 3, 4});
  auto d = at::randn({1, 0, 3, 4});
  std::vector<IValue> args_0{b, c10::MemoryFormat::Contiguous};
  std::vector<IValue> args_1{b, c10::MemoryFormat::Preserve};
  std::vector<IValue> args_2{c, c10::MemoryFormat::ChannelsLast};
  std::vector<IValue> args_3{d, c10::MemoryFormat::ChannelsLast};

  testStaticRuntime(clone_script_0, {a});
  testStaticRuntime(clone_script_0, {a}, {b});

  testStaticRuntime(clone_script_1, args_0);
  testStaticRuntime(clone_script_1, args_1);
  testStaticRuntime(clone_script_1, args_2);
  testStaticRuntime(clone_script_1, args_3);
  testStaticRuntime(clone_script_1, args_0, args_1);
  testStaticRuntime(clone_script_1, args_3, args_2);
}

TEST(StaticRuntime, Clamp) {
  const auto clamp_script_1 = R"JIT(
    def forward(self, inp: Tensor, min: int, max: int):
        a = torch.clamp(inp, min, max).clone()
        return (a)
  )JIT";

  const auto clamp_script_2 = R"JIT(
    def forward(self, inp: Tensor, min: Tensor, max: Tensor):
        a = torch.clamp(inp, min, max).clone()
        return (a)
  )JIT";
  auto a = at::randn({2, 3});
  auto max_t = at::full_like(a, 1);
  auto min_t = at::full_like(a, -1);

  auto b = at::randn({4, 3, 2});
  auto max_t1 = at::full_like(b, 1);
  auto min_t1 = at::full_like(b, -1);

  testStaticRuntime(clamp_script_1, {a, -1, 1});
  testStaticRuntime(clamp_script_2, {a, min_t, max_t});

  testStaticRuntime(clamp_script_1, {a, -1, 1}, {b, -1, 1});
  testStaticRuntime(clamp_script_2, {a, min_t, max_t}, {b, max_t1, min_t1});
}

TEST(StaticRuntime, Logit) {
  // no nnc
  const auto logit_script_1 = R"JIT(
    def forward(self, inp: Tensor):
        a = torch.logit(inp).clone()
        return (a)
  )JIT";

  // with nnc
  const auto logit_script_2 = R"JIT(
    def forward(self, inp: Tensor):
        a = torch.logit(inp, 1e-6).clone()
        return (a)
  )JIT";

  // no nnc
  const auto logit_script_3 = R"JIT(
    def forward(self, inp: Tensor, eps: float):
        a = torch.logit(inp, eps).clone()
        return (a)
  )JIT";
  auto a = at::ones({2, 3});
  double b = 1e-6;
  std::vector<IValue> args_1{a};
  std::vector<IValue> args_2({a, b});

  auto c = at::ones({4, 3, 2});

  // logit
  testStaticRuntime(logit_script_1, args_1);
  testStaticRuntime(logit_script_2, args_1);
  testStaticRuntime(logit_script_3, args_2);

  testStaticRuntime(logit_script_1, args_1, {c});
  testStaticRuntime(logit_script_2, args_1, {c});
  testStaticRuntime(logit_script_3, args_2, {c, b});
}

TEST(StaticRuntime, EmbeddingBag) {
  const std::string embedding_bag_default = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  const std::string embedding_bag_mean = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c, False, 1)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  const std::string embedding_bag_max = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c, False, 2)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  const std::string embedding_bag_sum_last_offset = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c, False, 0, False, None, True)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  const std::string embedding_bag_mean_last_offset = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c, False, 1, False, None, True)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  const std::string embedding_bag_max_last_offset = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        x, y, z, _ = torch.embedding_bag(a, b, c, False, 2, False, None, True)
        return (x.clone(), y.clone(), z.clone(), _.clone())
  )JIT";

  at::Tensor weight = torch::randn({3, 11}, at::ScalarType::Float);
  at::Tensor input = torch::tensor({0, 1, 0, 2});
  at::Tensor offset = torch::tensor({0, 2, 4});
  std::vector<IValue> args{weight, input, offset};
  testStaticRuntime(embedding_bag_default, args);
  testStaticRuntime(embedding_bag_mean, args);
  testStaticRuntime(embedding_bag_max, args);
  testStaticRuntime(embedding_bag_sum_last_offset, args);
  testStaticRuntime(embedding_bag_mean_last_offset, args);
  testStaticRuntime(embedding_bag_max_last_offset, args);

  at::Tensor weight2 = torch::randn({10, 11}, at::ScalarType::Float);
  at::Tensor input2 = torch::tensor({0, 1, 0, 2, 1});
  at::Tensor offset2 = torch::tensor({0, 1, 2, 3, 4, 5});
  std::vector<IValue> args2{weight2, input2, offset2};
  testStaticRuntime(embedding_bag_default, args, args2);
  testStaticRuntime(embedding_bag_mean, args, args2);
  testStaticRuntime(embedding_bag_max, args, args2);
  testStaticRuntime(embedding_bag_sum_last_offset, args, args2);
  testStaticRuntime(embedding_bag_mean_last_offset, args, args2);
  testStaticRuntime(embedding_bag_max_last_offset, args, args2);
}

TEST(StaticRuntime, EmbeddingBagWithManagedOutput) {
  const std::string embedding_bag_managed_output = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        # The outputs of embedding_bag become an intermediate tensors
        # since they are not directly returned from the graph.
        x, y, z, _ = torch.embedding_bag(a, b, c)
        return x + x
  )JIT";

  at::Tensor weight = torch::randn({3, 8}, at::ScalarType::Float);
  at::Tensor input = torch::tensor({0, 1, 0, 2});
  at::Tensor offset = torch::tensor({0, 2});
  std::vector<IValue> args{weight, input, offset};

  at::Tensor weight2 = torch::randn({6, 8}, at::ScalarType::Float);
  at::Tensor input2 = torch::tensor({0, 1, 0, 2, 3, 4});
  at::Tensor offset2 = torch::tensor({0, 2, 4, 5});
  std::vector<IValue> args2{weight2, input2, offset2};

  testStaticRuntime(embedding_bag_managed_output, args);
  testStaticRuntime(embedding_bag_managed_output, args, args2);
}

TEST(StaticRuntime, LayerNorm) {
  const std::string layer_norm_with_weights = R"JIT(
    def forward(self, input: Tensor, normalized_shape: List[int], weight: Tensor, bias: Tensor):
        return torch.layer_norm(input, normalized_shape, weight, bias, 1e-05, False).clone()
  )JIT";

  const std::string layer_norm_without_weights = R"JIT(
    def forward(self, input: Tensor, normalized_shape: List[int]):
        return torch.layer_norm(input, normalized_shape, None, None, 1e-05, False).clone()
  )JIT";

#ifdef FBCODE_CAFFE2
  script::Module module("module");
  module.define(layer_norm_with_weights);
  torch::jit::StaticModule smodule(module);
  ASSERT_EQ(getNodeWithKind(smodule, "aten::layer_norm"), nullptr);
  ASSERT_NE(getNodeWithKind(smodule, "static_runtime::layer_norm"), nullptr);
#endif
  const auto a = torch::rand({1, 2, 2, 2});
  const auto b = torch::rand({3, 2, 2, 2});
  for (int normalized_size : {2, 3}) {
    std::vector<int64_t> normalized_shape(normalized_size, 2);
    const auto weight = torch::rand(normalized_shape);
    const auto bias = torch::rand(normalized_shape);

    std::vector<IValue> args{a, normalized_shape, weight, bias};
    std::vector<IValue> args1{b, normalized_shape, weight, bias};
    testStaticRuntime(layer_norm_with_weights, args);
    testStaticRuntime(layer_norm_with_weights, args, args1);

    args = {a, normalized_shape};
    testStaticRuntime(layer_norm_without_weights, args);
    testStaticRuntime(layer_norm_without_weights, args, {b, normalized_shape});
  }
}

TEST(StaticRuntime, Bmm) {
  const auto bmm_script = R"JIT(
    def forward(self, inp: Tensor, mat2: Tensor):
      return torch.bmm(inp, mat2).clone()
  )JIT";

  auto a = at::randn({10, 4, 5});
  auto b = at::randn({10, 5, 6});

  auto c = at::randn({12, 5, 6});
  auto d = at::randn({12, 6, 7});

  std::vector<IValue> args{a, b};
  std::vector<IValue> args1{c, d};
  testStaticRuntime(bmm_script, args);
  testStaticRuntime(bmm_script, args1);
  testStaticRuntime(bmm_script, args, args1);
}

TEST(StaticRuntime, Addmm) {
  const auto addmm_script = R"JIT(
    def forward(self, inp: Tensor, mat1: Tensor, mat2: Tensor, beta: float, alpha: float):
      return torch.addmm(inp, mat1, mat2, alpha=alpha, beta=beta).clone()
  )JIT";
  auto inp1 = at::randn({5});
  auto mat1 = at::randn({3, 4});
  auto mat2 = at::randn({4, 5});

  auto inp2 = at::randn({3, 7});
  auto mat3 = at::randn({3, 6});
  auto mat4 = at::randn({6, 7});

  std::vector<IValue> args{inp1, mat1, mat2, 1.0, 2.0};
  std::vector<IValue> args1{inp2, mat3, mat4, 2.0, 1.0};
  testStaticRuntime(addmm_script, args);
  testStaticRuntime(addmm_script, args1);
  testStaticRuntime(addmm_script, args, args1);
}

TEST(StaticRuntime, Abs) {
  const auto abs_script = R"JIT(
    def forward(self, a):
      return a.abs().clone()
  )JIT";
  auto a = at::randn({2, 3});
  auto b = at::randn({4, 2, 3});
  std::vector<IValue> args{a};
  std::vector<IValue> args2{b};
  testStaticRuntime(abs_script, args);
  testStaticRuntime(abs_script, args, args2);
}

TEST(StaticRuntime, Binary) {
  const auto add_script = R"JIT(
    def forward(self, a, b):
        c = a + b
        return (c.clone())
  )JIT";

  const auto list_construct_script = R"JIT(
    def forward(self, a, b):
      return [a, b]
  )JIT";

  const auto list_construct_script_2 = R"JIT(
    def forward(self, a, b):
      c = a + a
      return [c, c]
  )JIT";

  const auto list_construct_script_3 = R"JIT(
    def forward(self, a, b):
      c = a + a
      return [c, c.flatten()]
  )JIT";

  const auto list_unpack_script = R"JIT(
    def forward(self, a, b):
      c = [a, b]
      x, y = c
      z = x + y
      return z.clone()
  )JIT";

  const auto list_unpack_script_2 = R"JIT(
    def forward(self, a, b):
      c = [a, b]
      x, y = c
      z = (x, y)
      return z
  )JIT";

  const auto tuple_construct_script = R"JIT(
    def forward(self, a, b):
      return (a, b)
  )JIT";

  const auto tuple_construct_script_2 = R"JIT(
    def forward(self, a, b):
      return (a.flatten(), b)
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::ones({2, 3});

  auto c = at::randn({4, 2, 3});
  auto d = at::ones({4, 2, 3});

  std::vector<IValue> args{a, b};

  testStaticRuntime(add_script, args);
  testStaticRuntime(add_script, args, {c, d});
  testStaticRuntime(list_construct_script, args);
  testStaticRuntime(list_construct_script_2, args);
  testStaticRuntime(list_construct_script_3, args);
  testStaticRuntime(list_unpack_script, args);
  testStaticRuntime(list_unpack_script_2, args);
  testStaticRuntime(tuple_construct_script, args);
  testStaticRuntime(tuple_construct_script_2, args);
}

TEST(StaticRuntime, MatMul) {
  const auto aten_matmul = R"JIT(
    def forward(self, a: Tensor, b: Tensor):
        return torch.matmul(a, b).clone()
  )JIT";

  // 1-D, 1-D
  std::vector<IValue> args{at::randn({3}), at::randn({3})};
  testStaticRuntime(aten_matmul, args);
  // 2-D, 2-D
  std::vector<IValue> args1 = {at::randn({3, 2}), at::randn({2, 3})};
  testStaticRuntime(aten_matmul, args1);
  // 1-D, 2-D
  std::vector<IValue> args2 = {at::randn({3}), at::randn({3, 5})};
  testStaticRuntime(aten_matmul, args2);
  // 2-D, 1-D
  std::vector<IValue> args3 = {at::randn({3, 5}), at::randn({5})};
  testStaticRuntime(aten_matmul, args3);
  // > 2-D , > 2-D
  std::vector<IValue> args4 = {at::randn({3, 1, 4, 5}), at::randn({2, 5, 6})};
  testStaticRuntime(aten_matmul, args4);

  testStaticRuntime(aten_matmul, args3, args4);
}

TEST(StaticRuntime, Sign) {
  const auto sign_tensor = R"JIT(
    def forward(self, input: Tensor):
        return torch.sign(input).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({4, 3, 2});

  std::vector<IValue> args{a};
  testStaticRuntime(sign_tensor, args);
  testStaticRuntime(sign_tensor, args, {b});
}

TEST(StaticRuntime, Div) {
  const auto div_tensor = R"JIT(
    def forward(self, a: Tensor, b: Tensor):
        return torch.div(a, b).clone()
  )JIT";

  const auto div_scalar = R"JIT(
    def forward(self, a: Tensor, b: int):
        return torch.div(a, b).clone()
  )JIT";

  const auto div_tensor_mode = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: str):
        return torch.div(a, b, rounding_mode=c).clone()
  )JIT";

  const auto div_scalar_mode = R"JIT(
    def forward(self, a: Tensor, b: float, c: str):
        return torch.div(a, b, rounding_mode=c).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({2, 3});
  auto c = at::randn({4, 3, 2});
  auto d = at::randn({4, 3, 2});

  std::vector<IValue> args0{a, b};
  testStaticRuntime(div_tensor, args0);
  testStaticRuntime(div_tensor, args0, {c, d});

  std::vector<IValue> args1{a, 3};
  testStaticRuntime(div_scalar, args1);
  testStaticRuntime(div_scalar, args1, {c, 4});

  std::vector<IValue> args2{a, b, "floor"};
  testStaticRuntime(div_tensor_mode, args2);
  testStaticRuntime(div_tensor_mode, args2, {c, d, "floor"});

  std::vector<IValue> args3{a, 2.3, "trunc"};
  testStaticRuntime(div_scalar_mode, args3);
  testStaticRuntime(div_scalar_mode, args3, {c, 1.5, "trunc"});
}

TEST(StaticRuntime, Mul) {
  const auto mul_tensor = R"JIT(
    def forward(self, a: Tensor, b: Tensor):
        return torch.mul(a, b).clone()
  )JIT";

  const auto mul_scalar = R"JIT(
    def forward(self, a: Tensor, b: int):
        return torch.mul(a, b).clone()
  )JIT";

  auto a = at::randn({3, 3});
  auto b = at::randn({3, 3});
  auto c = at::randn({3, 3, 3});
  auto d = at::randn({3, 3, 3});

  std::vector<IValue> tensor_args1{a, b};
  std::vector<IValue> tensor_args2{c, d};

  testStaticRuntime(mul_tensor, tensor_args1);
  testStaticRuntime(mul_tensor, tensor_args1, tensor_args2);

  std::vector<IValue> scalar_args1{a, 42};
  std::vector<IValue> scalar_args2{c, 42};

  testStaticRuntime(mul_scalar, scalar_args1);
  testStaticRuntime(mul_scalar, scalar_args1, scalar_args2);
}

TEST(StaticRuntime, Log) {
  const auto log_tensor = R"JIT(
    def forward(self, inp: Tensor):
        a = torch.log(inp).clone()
        return (a)
  )JIT";

  // Ensure that the input values are valid.
  auto a = at::abs(at::randn({2, 3}));
  auto b = at::abs(at::randn({4, 3, 2}));

  std::vector<IValue> args{a};
  testStaticRuntime(log_tensor, args);
  testStaticRuntime(log_tensor, args, {b});
}

TEST(StaticRuntime, Sub) {
  const auto sub_tensor = R"JIT(
    def forward(self, a: Tensor, b: Tensor):
        return torch.sub(a, b).clone()
  )JIT";

  const auto sub_scalar = R"JIT(
    def forward(self, a: Tensor, b: int):
        return torch.sub(a, b).clone()
  )JIT";

  const auto sub_tensor_alpha = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: float):
        return torch.sub(a, b, alpha=c).clone()
  )JIT";

  const auto sub_scalar_alpha = R"JIT(
    def forward(self, a: Tensor, b: float, c: int):
        return torch.sub(a, b, alpha=c).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({2, 3});
  auto c = at::randn({4, 3, 2});
  auto d = at::randn({4, 3, 2});

  std::vector<IValue> args0{a, b};
  testStaticRuntime(sub_tensor, args0);
  testStaticRuntime(sub_tensor, args0, {c, d});

  std::vector<IValue> args1{a, 3};
  testStaticRuntime(sub_scalar, args1);
  testStaticRuntime(sub_scalar, args1, {c, 4});

  std::vector<IValue> args2{a, b, 2.3};
  testStaticRuntime(sub_tensor_alpha, args2);
  testStaticRuntime(sub_tensor_alpha, {c, d, 3.1});

  std::vector<IValue> args3{a, 2.3, 4};
  testStaticRuntime(sub_scalar_alpha, args3);
  testStaticRuntime(sub_scalar_alpha, {c, 1.3, 2});
}

TEST(StaticRuntime, NanToNum) {
  const auto nan_to_num_script = R"JIT(
    def forward(self, a: Tensor, nan: float, posinf: float, neginf: float):
        return torch.nan_to_num(a, nan, posinf, neginf).clone()
  )JIT";

  const auto inf = std::numeric_limits<double>::infinity();
  const auto nan = std::numeric_limits<double>::quiet_NaN();

  auto a = torch::tensor({{1.0, nan}, {-inf, inf}});
  auto b = at::randn({3, 6});
  float* b_data = b.data_ptr<float>();
  b_data[0] = nan;
  b_data[4] = -inf;
  b_data[11] = inf;
  b_data[13] = nan;

  std::vector<IValue> args1{a, 1.0, 2.0, -2.0};
  std::vector<IValue> args2{b, 1.0, 2.0, -2.0};

  testStaticRuntime(
      nan_to_num_script,
      args1,
      /*args2*/ {},
      /*use_allclose*/ true,
      /*use_equalnan*/ true);
  testStaticRuntime(
      nan_to_num_script,
      args1,
      args2,
      /*use_allclose*/ true,
      /*use_equalnan*/ true);
}

TEST(StaticRuntime, Stack) {
  const auto stack_dim = R"JIT(
    def forward(self, a: Tensor, b: Tensor, dim: int):
        inputs = [a]
        inputs.append(b) # mutation to avoid using VarStack
        return torch.stack(inputs, dim = dim).clone()
  )JIT";

  const auto stack_three = R"JIT(
    def forward(self, a: Tensor, b: Tensor, c: Tensor):
        inputs = [a, b]
        inputs.append(c) # mutation to avoid using VarStack
        return torch.stack(inputs).clone()
  )JIT";

  auto a = at::randn({2, 2});
  auto b = at::randn({2, 2});
  auto c = at::randn({2, 2});

  auto d = at::randn({3, 3, 3});
  auto e = at::randn({3, 3, 3});
  auto f = at::randn({3, 3, 3});

  std::vector<IValue> args1_dim{a, b, 0};
  std::vector<IValue> args2_dim{d, e, 1};
  std::vector<IValue> args_dim_negative{d, e, -1};

  std::vector<IValue> args1_three_tensors{a, b, c};
  std::vector<IValue> args2_three_tensors{d, e, f};

  testStaticRuntime(stack_dim, args1_dim);
  testStaticRuntime(stack_dim, args1_dim, args2_dim);

  testStaticRuntime(stack_dim, args_dim_negative);

  testStaticRuntime(stack_three, args1_three_tensors);
  testStaticRuntime(stack_three, args1_three_tensors, args2_three_tensors);
}

TEST(StaticRuntime, ReLU) {
  const auto relu_script = R"JIT(
    def forward(self, a: Tensor):
        return torch.relu(a).clone()
  )JIT";
  auto a = at::randint(-10, 10, {2, 4});
  auto b = at::randint(-10, 10, {3, 6});

  std::vector<IValue> args1{a};
  std::vector<IValue> args2{b};

  testStaticRuntime(relu_script, args1);
  testStaticRuntime(relu_script, args1, args2);
}

TEST(StaticRuntime, Tanh) {
  const auto tanh_script = R"JIT(
    def forward(self, a):
        return torch.tanh(a).clone()
  )JIT";
  auto a = at::randn({2, 2});
  auto b = at::randn({3, 3, 3});

  std::vector<IValue> args1{a};
  std::vector<IValue> args2{b};

  testStaticRuntime(tanh_script, args1, /*args2*/ {}, /*use_allclose*/ true);
  testStaticRuntime(tanh_script, args1, args2, /*use_allclose*/ true);
}

TEST(StaticRuntime, Norm) {
  const auto norm_2arg = R"JIT(
    def forward(self, a: Tensor, p: int):
        return torch.norm(a, p).clone()
  )JIT";

  const auto norm_3arg = R"JIT(
    def forward(self, a: Tensor, p: int, dtype: int):
        return torch.norm(a, p, dtype=dtype).clone()
  )JIT";

  const auto norm_4arg = R"JIT(
    def forward(self, a: Tensor, p: int, dim: List[int], keepdim: bool):
        return torch.norm(a, p, dim, keepdim).clone()
  )JIT";

  const auto norm_5arg = R"JIT(
    def forward(self, a: Tensor, p: int, dim: List[int], keepdim: bool, dtype: int):
        return torch.norm(a, p, dim, keepdim, dtype=dtype).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({4, 3, 5});
  auto dim = std::vector<int64_t>({1});
  auto dtype = at::ScalarType::Float;

  std::vector<IValue> args2{a, 2};
  testStaticRuntime(norm_2arg, args2);
  testStaticRuntime(norm_2arg, args2, {b, 2}, false, false, false);

  std::vector<IValue> args3{a, 2, dtype};
  testStaticRuntime(norm_3arg, args3);
  testStaticRuntime(norm_3arg, args3, {b, 2, dtype}, false, false, false);

  std::vector<IValue> args4{a, 3, dim, false};
  testStaticRuntime(norm_4arg, args4);
  testStaticRuntime(norm_4arg, args4, {b, 3, dim, false});

  std::vector<IValue> args5{a, 4, dim, true, dtype};
  testStaticRuntime(norm_5arg, args5);
  testStaticRuntime(norm_5arg, args5, {b, 4, dim, true, dtype});
}

TEST(StaticRuntime, Reshape) {
  const auto reshape_script_1 = R"JIT(
    def forward(self, a: Tensor, shape: List[int]):
        b = a.reshape(shape)
        return b + b
  )JIT";

  const auto reshape_script_2 = R"JIT(
    def forward(self, a: Tensor, shape: List[int]):
        b = a.transpose(0, 1)
        return b.reshape(shape)
  )JIT";

  const auto reshape_script_3 = R"JIT(
    def forward(self, inp: Tensor, shape: List[int]):
        a = inp + inp
        b = a.reshape(shape)
        c = a.reshape(shape)
        d = c + c
        e = d + d
        f = e * e
        g = f * f
        return b.reshape(shape), g
  )JIT";

  // exercise reshape_copy and flatten_copy
  const auto reshape_script_4 = R"JIT(
    def forward(self, inp: Tensor, shape: List[int]):
        k = inp + inp
        a = k + k
        b = a.reshape(shape)
        c = a.flatten().reshape(shape)
        return b + c
  )JIT";

  // exercise reshape_copy
  const auto reshape_script_5 = R"JIT(
    def forward(self, inp: Tensor, shape: List[int]):
        a = inp + inp
        b = a.reshape(shape)
        c = a.reshape(shape).relu()
        d = c + c
        e = d + d
        f = e * e
        g = f * f
        return g
  )JIT";

  const auto reshape_inplace_script = R"JIT(
    def forward(self, inp: Tensor, shape: List[int]):
        a = inp + inp
        b = a.reshape(shape)
        c = b.sigmoid_()
        d = c + c
        e = a + a
        f = b + b
        return (d, e, f)
  )JIT";

  // b is in_contiguous
  const auto reshape_incontiguous_script = R"JIT(
    def forward(self, a: Tensor, shape: List[int]):
        b = a.transpose(0, 1)
        c = b.reshape(shape)
        c = c.relu()
        return (c)
  )JIT";

  auto a = at::randn({2, 3});
  auto b = std::vector<int64_t>({3, 2});
  std::vector<IValue> args{a, b};

  auto c = at::randn({4, 5});
  auto d = std::vector<int64_t>({5, 1, 2, 2});
  std::vector<IValue> args1{c, d};

  testStaticRuntime(reshape_script_1, args);
  testStaticRuntime(reshape_script_2, args);
  testStaticRuntime(reshape_script_3, args);
  testStaticRuntime(reshape_script_4, args);
  testStaticRuntime(reshape_script_5, args);
  testStaticRuntime(reshape_inplace_script, args);
  testStaticRuntime(reshape_incontiguous_script, args);

  testStaticRuntime(reshape_script_1, args, args1);
  testStaticRuntime(reshape_script_2, args, args1);
  testStaticRuntime(reshape_script_3, args, args1);
  testStaticRuntime(reshape_script_4, args, args1);
  testStaticRuntime(reshape_script_5, args, args1);
  testStaticRuntime(reshape_inplace_script, args, args1);
  testStaticRuntime(reshape_incontiguous_script, args, args1);
}

TEST(StaticRuntime, Repeat) {
  const std::string repeat = R"JIT(
    def forward(self, a: Tensor, repeats: List[int]):
        return torch.repeat(a, repeats).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({4, 3});
  auto c = std::vector<int64_t>({1, 2});
  auto d = std::vector<int64_t>({2, 3});
  std::vector<IValue> args1{a, c};
  std::vector<IValue> args2{b, d};

  testStaticRuntime(repeat, args1);
  testStaticRuntime(repeat, args2);
  testStaticRuntime(repeat, args1, args2);
}

TEST(StaticRuntime, Flatten) {
  // exercise flatten_copy
  const auto flatten_script_1 = R"JIT(
    def forward(self, a: Tensor, start_dim: int, end_dim: int):
        b = a * a
        c = torch.flatten(b, start_dim, end_dim)
        d = torch.relu(c)
        return d
  )JIT";

  const auto flatten_script_2 = R"JIT(
    def forward(self, a: Tensor, start_dim: int, end_dim: int):
        b = a.transpose(0, 1)
        return torch.flatten(b, start_dim, end_dim).clone()
  )JIT";

  auto test_flatten =
      [&](std::vector<int64_t> shape, int64_t start_dim, int64_t end_dim) {
        std::vector<int64_t> shape1(shape);
        if (shape1.size() > 0) {
          shape1[0] *= 6;
        }
        auto a = at::randn(shape);
        auto b = at::randn(shape1);
        std::vector<IValue> args{a, start_dim, end_dim};
        bool check_resize = shape1.size() > 0;
        testStaticRuntime(flatten_script_1, args);
        testStaticRuntime(
            flatten_script_1,
            args,
            {b, start_dim, end_dim},
            false, /* use_allclose */
            false, /* use_equalnan */
            check_resize);
        if (shape.size() > 2) {
          testStaticRuntime(flatten_script_2, args);
          testStaticRuntime(flatten_script_2, args, {b, start_dim, end_dim});
        }
      };

  test_flatten({2, 3}, 0, 1);
  test_flatten({2, 1, 3}, 1, 2);
  test_flatten({0, 1, 3, 0}, 1, 2);
  test_flatten({2, 3}, 1, 1);
  test_flatten({}, 0, 0);
}

TEST(StaticRuntime, pow) {
  const auto pow_script_ten_sca = R"JIT(
    def forward(self, input : Tensor, exponent : int):
        return torch.pow(input, exponent).clone()
  )JIT";

  const auto pow_script_ten_ten = R"JIT(
    def forward(self, input : Tensor, exponent : Tensor):
        return torch.pow(input, exponent).clone()
  )JIT";

  const auto pow_script_sca_ten = R"JIT(
    def forward(self, input : int, exponent : Tensor):
        return torch.pow(input, exponent).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({2, 3});
  auto c = at::randn({4, 3, 2});
  auto d = at::randn({4, 3, 2});

  std::vector<IValue> args0{a, 4};
  testStaticRuntime(pow_script_ten_sca, args0);
  testStaticRuntime(pow_script_ten_sca, args0, {c, 4});

  std::vector<IValue> args1{at::abs(a), b};
  testStaticRuntime(pow_script_ten_ten, args1);
  testStaticRuntime(pow_script_ten_ten, args1, {at::abs(c), d});

  std::vector<IValue> args2{5, b};
  testStaticRuntime(pow_script_sca_ten, args2);
  testStaticRuntime(pow_script_sca_ten, args2, {3, d});
}

TEST(StaticRuntime, to) {
  const auto to_script_dtype = R"JIT(
    def forward(self, input: Tensor, dtype: int, non_blocking: bool, copy: bool, memory_format: int):
        a = input + input
        return torch.to(a, dtype, non_blocking, copy, memory_format).clone()
  )JIT";

  const auto to_script_dtype_strided = R"JIT(
    def forward(self, input: Tensor, dtype: int, non_blocking: bool, copy: bool, memory_format: int):
        b = input.permute(0, 2, 3, 1)
        return torch.to(b, dtype, non_blocking, copy, memory_format).clone()
  )JIT";

  const auto to_script_prim_dtype = R"JIT(
    def forward(self, input:Tensor, dtype: Optional[int], non_blocking: bool, copy: bool):
        a = input + input
        return torch.to(a, dtype, non_blocking, copy).clone()
  )JIT";

  const auto to_script_other = R"JIT(
    def forward(self, input:Tensor, other: Tensor, non_blocking: bool, copy: bool, memory_format: int):
        a = input + input
        return torch.to(a, other, non_blocking, copy, memory_format).clone()
  )JIT";

  // if input is float tensor, b could be alias of a
  const auto to_script_alias = R"JIT(
    def forward(self, input:Tensor):
        a = input + input
        b = a.float()
        c = b * b
        return (c)
  )JIT";

  const auto to_script_fails_managed_output_check = R"JIT(
    def forward(self, a, b):
        d = a.half() * b.half()
        e = d.float()
        return e
  )JIT";

  const auto to_script_memory_planning_fail = R"JIT(
    def forward(self, a, b):
        d = a.half() * b.half()
        e = d.float().relu()
        return e
  )JIT";

  auto test_to = [&](at::ScalarType b, bool c, bool d, c10::MemoryFormat e) {
    auto a = at::randn({4, 3, 1, 2});
    auto other = at::randn({4, 3, 1, 2}).to(b);
    auto a2 = at::randn({3, 2, 2, 4});
    auto a2_other = at::randn({3, 2, 2, 4}).to(b);

    std::vector<IValue> args0{a, b, c, d, e};
    std::vector<IValue> args1{a, b, c, d};
    std::vector<IValue> args2{a, other, c, d, e};
    std::vector<IValue> args3{a, c10::nullopt, c, d};

    testStaticRuntime(to_script_dtype, args0);
    testStaticRuntime(to_script_dtype_strided, args0);
    testStaticRuntime(to_script_prim_dtype, args1);
    if (!d) {
      testStaticRuntime(to_script_prim_dtype, args3);
    }
    testStaticRuntime(to_script_other, args2);
    testStaticRuntime(to_script_alias, {a});
    testStaticRuntime(to_script_memory_planning_fail, {a, a});
    testStaticRuntime(to_script_fails_managed_output_check, {a, a});

    // dynamic shapes
    testStaticRuntime(to_script_dtype, args0, {a2, b, c, d, e});
    testStaticRuntime(to_script_dtype_strided, args0, {a2, b, c, d, e});
    testStaticRuntime(to_script_prim_dtype, args1, {a2, b, c, d});
    if (!d) {
      testStaticRuntime(to_script_prim_dtype, args3, {a2, c10::nullopt, c, d});
    }
    testStaticRuntime(to_script_other, args2, {a2, a2_other, c, d, e});
    testStaticRuntime(to_script_alias, {a}, {a2});
  };
  for (const bool non_blocking : {false, true}) {
    for (const bool copy : {false, true}) {
      // float->float, NCHW->NHWC
      test_to(
          at::ScalarType::Float,
          non_blocking,
          copy,
          c10::MemoryFormat::ChannelsLast);
      // float->half
      test_to(
          at::ScalarType::Half,
          non_blocking,
          copy,
          c10::MemoryFormat::Preserve);
      // float->float
      test_to(
          at::ScalarType::Float,
          non_blocking,
          copy,
          c10::MemoryFormat::Contiguous);
      test_to(
          at::ScalarType::Bool,
          non_blocking,
          copy,
          c10::MemoryFormat::Contiguous);
      // TODO: check if fbgemm is enabled properly in this case
      // half->float, NCHW->NHWC
      test_to(
          at::ScalarType::Half,
          non_blocking,
          copy,
          c10::MemoryFormat::ChannelsLast);
    }
  }
}

TEST(StaticRuntime, ExpandAs) {
  const auto expand_as_script = R"JIT(
    def forward(self, input: Tensor, other:Tensor):
        a = input.expand_as(other)
        return a.clone()
  )JIT";

  auto a = at::randn({3, 1});
  auto b = at::randn({3, 2});
  auto c = at::randn({4, 1});
  auto d = at::randn({4, 2});
  std::vector<IValue> args{a, b};
  std::vector<IValue> args2{c, d};
  testStaticRuntime(expand_as_script, args);
  testStaticRuntime(expand_as_script, args, args2);
}

TEST(StaticRuntime, Full) {
  const auto full_script = R"JIT(
    def forward(self,
                size: List[int],
                fill_value: int,
                dtype: Optional[int],
                layout: Optional[int],
                device: Optional[Device],
                pin_memory: Optional[bool]):
        a = torch.full(size,
                      fill_value,
                      dtype=dtype,
                      layout=layout,
                      device=device,
                      pin_memory=pin_memory)
        return (a.clone())
  )JIT";

  auto dtype = at::ScalarType::Int;
  auto cpu = at::Device(DeviceType::CPU);
  c10::List<int64_t> size0{2, 5};
  std::vector<IValue> args{size0, 4, dtype, at::kStrided, cpu, false};
  c10::List<int64_t> size1{5, 6};
  std::vector<IValue> args2{size1, 5, dtype, at::kStrided, cpu, false};
  testStaticRuntime(full_script, args);
  testStaticRuntime(full_script, args, args2);
}

TEST(StaticRuntime, FullLike) {
  const auto full_like_script = R"JIT(
    def forward(self,
                a: Tensor,
                fill_value: int,
                dtype: Optional[int],
                layout: Optional[int],
                device: Optional[Device],
                pin_memory: Optional[bool],
                memory_format: Optional[int]):
        b = torch.full_like(a,
                            fill_value,
                            dtype=dtype,
                            layout=layout,
                            device=device,
                            pin_memory=pin_memory,
                            memory_format=memory_format)
        return (b.clone())
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({3, 4, 2});
  auto dtype = at::ScalarType::Int;
  auto cpu = at::Device(DeviceType::CPU);
  std::vector<IValue> args{
      a, 4, dtype, at::kStrided, cpu, false, c10::MemoryFormat::Contiguous};
  std::vector<IValue> args2{
      b, 4, dtype, at::kStrided, cpu, false, c10::MemoryFormat::Contiguous};
  testStaticRuntime(full_like_script, args);
  testStaticRuntime(full_like_script, args, args2);
}

TEST(StaticRuntime, Linear) {
  const auto linear_script = R"JIT(
    def forward(self, inp: Tensor, weights: Tensor, bias: Optional[Tensor]) -> Tensor:
        return torch.linear(inp, weights, bias).clone()
  )JIT";

  auto input = at::randn({1, 2});
  auto weights = at::randn({1, 2});
  auto bias = at::randn({1, 1});

  std::vector<IValue> args{input, weights, bias};
  std::vector<IValue> args_no_bias{input, weights, c10::nullopt};

  auto input2 = at::randn({6, 3});
  auto weights2 = at::randn({6, 3});
  auto bias2 = at::randn({6, 6});

  std::vector<IValue> args2{input2, weights2, bias2};
  std::vector<IValue> args2_no_bias{input2, weights2, c10::nullopt};

  testStaticRuntime(linear_script, args);
  testStaticRuntime(linear_script, args_no_bias);

  testStaticRuntime(linear_script, args, args2);
  testStaticRuntime(linear_script, args, args2_no_bias);
}

TEST(StaticRuntime, VarCat) {
  const auto var_cat_script = R"JIT(
    def forward(self, inp1: Tensor, inp2: Tensor, dim: int):
      return torch.cat([inp1, inp2], dim).clone()
  )JIT";

  // 2D tensors - cat dim = 0
  std::vector<IValue> args1 = {at::randn({4, 6}), at::randn({5, 6}), 0};
  testStaticRuntime(var_cat_script, args1);

  // 3D tensors - cat dim = 1
  std::vector<IValue> args2 = {at::randn({4, 5, 6}), at::randn({4, 8, 6}), 1};
  testStaticRuntime(var_cat_script, args2);

  // 3D tensors - cat dim = 2
  std::vector<IValue> args3 = {at::randn({4, 5, 6}), at::randn({4, 5, 7}), 2};
  testStaticRuntime(var_cat_script, args3);

  // Negative dim
  std::vector<IValue> args4 = {at::randn({4, 5, 6}), at::randn({4, 5, 7}), -1};
  testStaticRuntime(var_cat_script, args4);

  testStaticRuntime(var_cat_script, args1, args2);
}

TEST(StaticRuntime, LeakyReLU) {
  torch::jit::Module mod = getLeakyReLUConstScriptModel();
  auto inputs = torch::randn({2, 2});

  // run jit graph executor
  std::vector<at::IValue> input_ivalues({inputs});
  at::Tensor output_1 = mod.forward(input_ivalues).toTensor();

  // run static runtime
  std::vector<c10::IValue> input_tensors({inputs});
  torch::jit::StaticModule smod(mod);
  at::Tensor output_2 = smod(input_tensors, {}).toTensor();
  smod.runtime().check_for_memory_leak();
  EXPECT_TRUE(torch::allclose(output_1, output_2, 1e-6));
}

static ProcessedNodeInputs createProcessedNodeInputs(
    c10::ArrayRef<uint16_t> inputs) {
  ProcessedNodeInputs result(inputs.size());
  for (const auto idx : c10::irange(inputs.size())) {
    result[idx] = inputs[idx];
  }
  return result;
}

static void checkProcessedNodeInputs(
    const ProcessedNodeInputs& io,
    c10::ArrayRef<uint16_t> inputs) {
  ASSERT_EQ(inputs.size(), io.size());
  for (const auto idx : c10::irange(inputs.size())) {
    EXPECT_EQ(inputs[idx], io[idx]);
  }
}

static void testProcessedNodeInputsRoundTrip(c10::ArrayRef<uint16_t> inputs) {
  auto io = createProcessedNodeInputs(inputs);
  checkProcessedNodeInputs(io, inputs);

  ProcessedNodeInputs copied(io);
  checkProcessedNodeInputs(copied, inputs);
  ProcessedNodeInputs moved(std::move(io));
  checkProcessedNodeInputs(moved, inputs);
}

TEST(ProcessedNodeInputs, Basic) {
  std::vector<std::vector<uint16_t>> testCases = {
      {}, // empty
      {0xABCD, 0x5a5a}, // inline
      {0x11, 0x22, 0x33, 0x44, 0x55}, // max inline size
      {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, // minimum outline size
      std::vector<uint16_t>(100, 0x5a), // large outline size
  };

  for (const auto& values : testCases) {
    testProcessedNodeInputsRoundTrip(values);
    for (const auto& values2 : testCases) {
      auto from = createProcessedNodeInputs(values);
      auto to = createProcessedNodeInputs(values2);

      to = from;
      checkProcessedNodeInputs(to, values);

      auto toMoveInto = createProcessedNodeInputs(values2);
      toMoveInto = std::move(from);
      checkProcessedNodeInputs(toMoveInto, values);
    }
  }
}

TEST(StaticRuntime, isinstance) {
  const auto isinstance_int_script = R"JIT(
    def forward(self, a: Any):
        return isinstance(a, int)
  )JIT";

  const auto isinstance_tensor_script = R"JIT(
    def forward(self, a: Any):
        return isinstance(a, torch.Tensor)
  )JIT";

  const auto isinstance_many_types_script = R"JIT(
    def forward(self, a: Any):
        return isinstance(a, (bool, int))
  )JIT";

  auto a = at::randn({2, 2});
  auto b = at::randn({2, 2, 2});

  std::vector<at::IValue> args{a};
  std::vector<at::IValue> args2{b};

  testStaticRuntime(isinstance_int_script, args);
  testStaticRuntime(isinstance_int_script, args, args2);

  testStaticRuntime(isinstance_tensor_script, args);
  testStaticRuntime(isinstance_tensor_script, args, args2);

  testStaticRuntime(isinstance_many_types_script, args);
  testStaticRuntime(isinstance_many_types_script, args, args2);
}

TEST(StaticRuntime, TypeCheck) {
  const auto typecheck_ir = R"IR(
  graph(%a.1 : Tensor,
        %b.1 : Tensor):
    %t0 : Float(2, 2, strides=[2, 1], device=cpu), %t1 : Float(3, 3, strides=[3, 1]), %type_matched : bool = prim::TypeCheck[types=[Float(2, 2, strides=[2, 1], device=cpu), Float(3, 3, strides=[3, 1])]](%a.1, %b.1)
    return (%t0, %t1, %type_matched)
  )IR";

  auto a = at::zeros({2, 2}, at::kFloat);
  a.to(at::kCPU);
  auto b = at::ones({3, 3}, at::kFloat);
  auto c = at::ones({2, 2, 2}, at::kFloat);

  std::vector<IValue> args_correct = {a, b};
  std::vector<IValue> args_incorrect = {a, c};

  testStaticRuntime(typecheck_ir, args_correct);
  testStaticRuntime(typecheck_ir, args_correct, args_incorrect);
}

TEST(StaticRuntime, Index) {
  const auto index_without_none_script = R"JIT(
    def forward(self, a: Tensor, idx: Tensor):
        return a[idx].clone()
  )JIT";

  // Index with boolean mask
  auto a = at::arange(4, at::kFloat).view({2, 2});
  auto idx_a = torch::tensor({{0, 1}, {0, 0}}, at::kBool);
  std::vector<IValue> args_a{a, idx_a};

  // Index with tensor
  auto b = at::arange(27, at::kFloat).view({3, 3, 3});
  auto idx_b = torch::tensor({0, 1, 2}, at::kLong);
  std::vector<IValue> args_b{b, idx_b};

  testStaticRuntime(index_without_none_script, args_a);
  testStaticRuntime(index_without_none_script, args_a, args_b);

  const auto index_with_none_script = R"JIT(
    def forward(self, a: Tensor, idx: Tensor, none: Optional[Tensor]):
        return a[idx, none].clone()
  )JIT";

  // Index with None
  // When indexing with none, the shape of `f` becomes [2, 1, 2],
  // so the mask must be reshaped appropriately.
  auto f = at::arange(4, at::kFloat).view({2, 1, 2});
  auto idx_f_reshape = torch::tensor({{{0, 1}}, {{0, 0}}}, at::kBool);
  std::vector<IValue> args_f_with_none{f, idx_f_reshape};
  args_f_with_none.emplace_back();

  testStaticRuntime(index_with_none_script, args_f_with_none);
  testStaticRuntime(
      index_with_none_script,
      args_f_with_none,
      {IValue(b), IValue(idx_b), IValue()});

  const auto index_with_two_tensors_script = R"JIT(
    def forward(self, a: Tensor, idx_a: Tensor, idx_b: Tensor):
        return a[idx_a, idx_b].clone()
  )JIT";

  // Index with multiple tensors
  const auto& c = a; // 2x2 tensor
  auto idx_c1 = torch::tensor({0, 0}, at::kLong);
  auto idx_c2 = torch::tensor({0}, at::kLong);
  std::vector<IValue> args_c{c, idx_c1, idx_c2};

  const auto& d = b; // 3x3x3 tensor
  auto idx_d1 = torch::tensor({{0, 0, 2}, {0, 1, 1}}, at::kLong);
  auto idx_d2 = torch::tensor({{1, 1, 0}, {1, 0, 2}}, at::kLong);
  std::vector<IValue> args_d{d, idx_d1, idx_d2};

  testStaticRuntime(index_with_two_tensors_script, args_c, args_d);
}

TEST(StaticRuntime, ClampMin) {
  const auto clamp_min_int_script = R"JIT(
    def forward(self, a: Tensor, b: int):
        return torch.clamp_min(a, b).clone()
  )JIT";

  const auto clamp_min_float_script = R"JIT(
    def forward(self, a: Tensor, b: float):
        return torch.clamp_min(a, b).clone()
  )JIT";

  auto a = at::randn({2, 2});
  auto b = at::randn({3, 3, 3});
  int scalar_int = 1;
  float scalar_float = 3.14;

  std::vector<IValue> args_a_int{a, scalar_int};
  std::vector<IValue> args_b_int{b, scalar_int};

  testStaticRuntime(clamp_min_int_script, args_a_int);
  testStaticRuntime(clamp_min_int_script, args_a_int, args_b_int);

  std::vector<IValue> args_a_float{a, scalar_float};
  std::vector<IValue> args_b_float{b, scalar_float};

  testStaticRuntime(clamp_min_float_script, args_a_float);
  testStaticRuntime(clamp_min_float_script, args_a_float, args_b_float);
}

TEST(StaticRuntime, Argmin) {
  const auto argmin_script = R"JIT(
    def forward(self, a: Tensor):
        return torch.argmin(a).clone()
  )JIT";

  const auto argmin_with_dim_script = R"JIT(
    def forward(self, a: Tensor, dim: int):
        return torch.argmin(a, dim).clone()
  )JIT";

  const auto argmin_with_keep_dim_script = R"JIT(
    def forward(self, a: Tensor, dim: int):
        return torch.argmin(a, dim, True).clone()
  )JIT";

  auto a = at::randn({2, 2});
  auto b = at::randn({17, 2, 1});

  testStaticRuntime(argmin_script, {a});
  testStaticRuntime(
      argmin_script,
      {a},
      {b},
      /* use_allclose */ false,
      /* use_equalnan */ false,
      /* check_resize */ false);

  int dim_a = 0;
  int dim_b = 1;

  std::vector<IValue> args_a{a, dim_a};
  std::vector<IValue> args_b{b, dim_b};

  testStaticRuntime(argmin_with_dim_script, args_a);
  testStaticRuntime(argmin_with_dim_script, args_a, args_b);

  testStaticRuntime(argmin_with_keep_dim_script, args_a);
  testStaticRuntime(argmin_with_keep_dim_script, args_a, args_b);
}

TEST(StaticRuntime, Softmax) {
  const auto softmax_script = R"JIT(
    def forward(self, a: Tensor, dim: int):
        return torch.softmax(a, dim).clone()
  )JIT";

  const auto softmax_script_with_dtype = R"JIT(
    def forward(self, a: Tensor, dim: int, dtype: int):
        return torch.softmax(a, dim, dtype=dtype).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto b = at::randn({3, 3, 3});

  testStaticRuntime(softmax_script, {a, 0});
  testStaticRuntime(softmax_script, {a, 1});

  testStaticRuntime(softmax_script, {b, 0});
  testStaticRuntime(softmax_script, {b, 1});
  testStaticRuntime(softmax_script, {b, 2});

  testStaticRuntime(softmax_script_with_dtype, {a, 1, at::ScalarType::Float});
  testStaticRuntime(softmax_script_with_dtype, {b, 1, at::ScalarType::Float});
}

TEST(StaticRuntime, GetItem_Dict) {
  const auto getitem_dict_tensor_script = R"JIT(
    def forward(self, key: Tensor):
        d = {key: 1}
        return d[key]
  )JIT";

  const auto getitem_dict_int_script = R"JIT(
    def forward(self, key: int):
        d = {key: 1}
        return d[key]
  )JIT";

  const auto getitem_dict_str_script = R"JIT(
    def forward(self, key: str):
        d = {key: 1}
        return d[key]
  )JIT";

  int int_key = 0;
  std::string str_key = "str";

  // No need to test these multiple times, args are not tensors
  testStaticRuntime(getitem_dict_int_script, {int_key});
  testStaticRuntime(getitem_dict_str_script, {str_key});

  auto a = torch::tensor({1});
  auto b = torch::tensor({1, 1});

  testStaticRuntime(getitem_dict_tensor_script, {a});
  testStaticRuntime(getitem_dict_tensor_script, {a}, {b});
}

TEST(StaticRuntime, GetItem_List) {
  const auto getitem_list_int_script = R"JIT(
    def forward(self, idx: int):
        lst = [1, 2, 3]
        return lst[idx]
  )JIT";

  const auto getitem_list_tensor_script = R"JIT(
    def forward(self, tensor: Tensor, idx: int):
        lst = [tensor, tensor]
        return lst[idx]
  )JIT";

  testStaticRuntime(getitem_list_int_script, {1});
  testStaticRuntime(getitem_list_int_script, {-1});

  auto a = torch::tensor({1});
  auto b = torch::tensor({1, 1});

  testStaticRuntime(getitem_list_tensor_script, {a, 1});
  testStaticRuntime(getitem_list_tensor_script, {a, 1}, {b, -1});
}

TEST(StaticRuntime, Transpose) {
  const auto transpose_script = R"JIT(
    def forward(self, a: Tensor, dim1: int, dim2: int):
        return torch.transpose(a, dim1, dim2).clone()
  )JIT";

  auto a = at::randn({2, 2});
  int dim1_a = 0;
  int dim2_a = 1;
  std::vector<IValue> args_a{a, dim1_a, dim2_a};

  auto b = at::randn({3, 3, 3});
  int dim1_b = 0;
  int dim2_b = 2;
  std::vector<IValue> args_b{b, dim1_b, dim2_b};

  testStaticRuntime(transpose_script, args_a);
  testStaticRuntime(transpose_script, args_a, args_b);
}

TEST(StaticRuntime, Permute) {
  const auto permute_script = R"JIT(
    def forward(self, a: Tensor, dims: List[int]):
        return torch.permute(a, dims).clone()
  )JIT";

  auto a = at::randn({2, 2});
  c10::List<int64_t> dims_a{1, 0};
  std::vector<IValue> args_a{a, dims_a};

  auto b = at::randn({3, 3, 3});
  c10::List<int64_t> dims_b{0, 2, 1};
  std::vector<IValue> args_b{b, dims_b};

  testStaticRuntime(permute_script, args_a);
  testStaticRuntime(permute_script, args_a, args_b);
}

TEST(StaticRuntime, Slice) {
  const auto slice_script = R"JIT(
    def forward(self, a: Tensor, dim: int, start: int, end: int, step: int):
      return a.slice(dim, start, end, step).clone()
  )JIT";

  auto a = at::randn({2, 2});
  int dim_a = 1;
  int start_a = 0;
  int end_a = 1;
  int step_a = 1;
  std::vector<IValue> args_a{a, dim_a, start_a, end_a, step_a};

  auto b = at::randn({3, 3, 3});
  int dim_b = 2;
  int start_b = 0;
  int end_b = 1;
  int step_b = 2;
  std::vector<IValue> args_b{b, dim_b, start_b, end_b, step_b};

  testStaticRuntime(slice_script, args_a);
  testStaticRuntime(slice_script, args_a, args_b);
}

TEST(StaticRuntime, Narrow) {
  const auto narrow_with_int_script = R"JIT(
    def forward(self, a: Tensor, dim: int, start: int, length: int):
        return a.narrow(dim, start, length).clone()
  )JIT";

  auto a = at::randn({5, 5});
  int dim_a = 0;
  int start_a_int = 3;
  int len_a = 2;
  std::vector<IValue> args_a{a, dim_a, start_a_int, len_a};

  auto b = at::randn({5, 5, 5});
  int dim_b = 1;
  int start_b_int = 2;
  int len_b = 3;
  std::vector<IValue> args_b{b, dim_b, start_b_int, len_b};

  testStaticRuntime(narrow_with_int_script, args_a);
  testStaticRuntime(narrow_with_int_script, args_a, args_b);
}

TEST(StaticRuntime, TupleUnpack) {
  const auto two_tuple_unpack_script = R"JIT(
    def forward(self, tup: Tuple[Tensor, Tensor]):
        a, b = tup
        return (a, b)
  )JIT";

  const auto three_tuple_unpack_script = R"JIT(
    def forward(self, tup: Tuple[Tensor, Tensor, Tensor]):
        a, b, c = tup
        return (a, b, c)
  )JIT";

  auto two_tup = c10::ivalue::Tuple::create({at::randn({1}), at::randn({1})});
  auto two_tup_large =
      c10::ivalue::Tuple::create({at::randn({2, 2}), at::randn({2, 2})});

  auto three_tup = c10::ivalue::Tuple::create(
      {at::randn({1}), at::randn({1}), at::randn({1})});
  auto three_tup_large = c10::ivalue::Tuple::create(
      {at::randn({2, 2}), at::randn({2, 2}), at::randn({2, 2})});

  testStaticRuntime(two_tuple_unpack_script, {two_tup});
  testStaticRuntime(two_tuple_unpack_script, {two_tup}, {two_tup_large});

  testStaticRuntime(three_tuple_unpack_script, {three_tup});
  testStaticRuntime(three_tuple_unpack_script, {three_tup}, {three_tup_large});
}

TEST(StaticRuntime, Append) {
  const auto append_int_script = R"JIT(
    def forward(self, a: int):
        lst = [1, 2, 3]
        lst.append(a)
        return lst
  )JIT";

  const auto append_tensor_script = R"JIT(
    def forward(self, a: Tensor):
        lst = []
        lst.append(a)
        return lst
  )JIT";

  std::vector<IValue> args_int{1};

  testStaticRuntime(append_int_script, args_int);

  std::vector<IValue> args_tensor{at::randn({1})};
  std::vector<IValue> args_tensor_large{at::randn({2, 2})};

  testStaticRuntime(append_tensor_script, args_tensor);
  testStaticRuntime(append_tensor_script, args_tensor, args_tensor_large);
}

TEST(StaticRuntime, QuantizedLinear) {
  const std::string quantize_script = R"IR(
    graph(%input: Tensor, %weights: Tensor):
        %scale: float = prim::Constant[value=1.]()
        %zero_point: int = prim::Constant[value=1]()
        %bias: None = prim::Constant()
        %packed_params = quantized::linear_prepack(%weights, %bias)
        %1254 = quantized::linear(%input, %packed_params, %scale, %zero_point)
        %1249: Tensor = aten::dequantize(%1254)
        return (%1249)
  )IR";
  at::Tensor weight =
      at::quantize_per_tensor(torch::randn({3, 2}), 2, 3, torch::kQInt8);
  at::Tensor input =
      at::quantize_per_tensor(torch::randn({3, 2}), 2, 3, torch::kQUInt8);

  at::Tensor weight_2 =
      at::quantize_per_tensor(torch::randn({8, 3}), 2, 3, torch::kQInt8);
  at::Tensor input_2 =
      at::quantize_per_tensor(torch::randn({9, 3}), 2, 3, torch::kQUInt8);

  testStaticRuntime(quantize_script, {input, weight}, {input_2, weight_2});
}

TEST(StaticRuntime, QuantizedLinearDynamicFp16) {
  const std::string quantized_linear_dynamic_fp16_script = R"IR(
    graph(%input: Tensor, %weights: Tensor):
        %bias: None = prim::Constant()
        %packed_params = quantized::linear_prepack_fp16(%weights, %bias)
        %output = quantized::linear_dynamic_fp16(%input, %packed_params)
        %ret = aten::clone(%output, %bias)
        return (%output)
  )IR";
  at::Tensor weight = torch::randn({3, 2}, torch::kFloat);
  at::Tensor input = torch::randn({3, 2}, torch::kFloat);

  at::Tensor weight_2 = torch::randn({4, 3}, torch::kFloat);
  at::Tensor input_2 = torch::randn({5, 3}, torch::kFloat);

  testStaticRuntime(
      quantized_linear_dynamic_fp16_script,
      {input, weight},
      {input_2, weight_2});
}

TEST(StaticRuntime, VarStack) {
  const auto var_stack_script = R"JIT(
    def forward(self, inp1: Tensor, inp2: Tensor, dim: int):
        return torch.stack([inp1, inp2], dim).clone()
  )JIT";

  // 2D tensors - stack dim = 0
  std::vector<IValue> args1 = {at::randn({6, 6}), at::randn({6, 6}), 0};
  testStaticRuntime(var_stack_script, args1);

  // 3D tensors - stack dim = 1
  std::vector<IValue> args2 = {at::randn({4, 5, 6}), at::randn({4, 5, 6}), 1};
  testStaticRuntime(var_stack_script, args2);

  // 3D tensors - stack dim = 2
  std::vector<IValue> args3 = {at::randn({4, 5, 6}), at::randn({4, 5, 6}), 2};
  testStaticRuntime(var_stack_script, args3);

  // Negative dim
  std::vector<IValue> args4 = {at::randn({4, 5, 6}), at::randn({4, 5, 6}), -1};
  testStaticRuntime(var_stack_script, args4);

  // Non-serial path
  std::vector<IValue> args5 = {at::randn({1, 2, 3}), at::randn({1, 2, 3}), 3};
  testStaticRuntime(var_stack_script, args5);

  testStaticRuntime(var_stack_script, args1, args2);
}

TEST(StaticRuntime, FmodTensor) {
  const auto fmod_tensor = R"JIT(
    def forward(self, a: Tensor, b: Tensor):
        return torch.fmod(a, b).clone()
  )JIT";

  // fmod tensor version
  auto a = at::randn({2, 3});
  auto b = at::randn({2, 3});
  std::vector<IValue> args0{a, b};
  testStaticRuntime(fmod_tensor, args0);

  // check for dynamic shapes
  auto c = at::randn({4, 3, 2});
  auto d = at::randn({4, 3, 2});
  std::vector<IValue> args1{c, d};
  testStaticRuntime(fmod_tensor, args0, args1);
}

TEST(StaticRuntime, FmodScalar) {
  const auto fmod_scalar = R"JIT(
    def forward(self, a: Tensor, b: int):
        return torch.fmod(a, b).clone()
  )JIT";

  auto a = at::randn({2, 3});

  // fmod scalar version
  std::vector<IValue> args2{a, 3};
  testStaticRuntime(fmod_scalar, args2);

  // check for dynamic shapes
  auto c = at::randn({4, 3, 2});
  std::vector<IValue> args3{c, 4};
  testStaticRuntime(fmod_scalar, args2, args3);

  // test int32 version
  a = at::randint(-100, 100, {2, 3}, at::kInt);
  c = at::randint(-100, 100, {4, 3, 2}, at::kInt);
  testStaticRuntime(fmod_scalar, {a, 3});
  testStaticRuntime(fmod_scalar, {a, 3}, {c, 4});
}

TEST(StaticRuntime, QEmbeddingBagByteUnpack) {
  const std::string embedding_bag_byte_prepack_script = R"IR(
    graph(%input: Tensor):
        %none : None = prim::Constant()
        %output: Tensor = quantized::embedding_bag_byte_prepack(%input)
        %res: Tensor = aten::clone(%output, %none)
        return (%res)
  )IR";

  auto a = torch::randn({8, 16}, at::ScalarType::Float);
  auto b = torch::randn({8 * 2, 16 * 2}, at::ScalarType::Float);

  testStaticRuntime(embedding_bag_byte_prepack_script, {a});
  testStaticRuntime(embedding_bag_byte_prepack_script, {a}, {b});
}

TEST(StaticRuntime, LinalgNorm_ScalarOrd) {
  const auto linalg_norm_ord_scalar = R"JIT(
    def forward(self, a: Tensor, ord: int, dim: List[int], keepdim: bool, dtype: int):
        return torch.linalg_norm(a, ord, dim, keepdim, dtype=dtype).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto dim = std::vector<int64_t>({1});
  auto dtype = at::ScalarType::Float;

  std::vector<IValue> args0{a, 4, dim, true, dtype};
  testStaticRuntime(linalg_norm_ord_scalar, args0);

  auto b = at::randn({3, 2, 6});
  std::vector<IValue> args1{b, 4, dim, true, dtype};
  testStaticRuntime(linalg_norm_ord_scalar, args0, args1);
}

TEST(StaticRuntime, LinalgNorm_StringOrd) {
  const auto linalg_norm_ord_str = R"JIT(
    def forward(self, a: Tensor, ord: str, dim: List[int], keepdim: bool, dtype: int):
        return torch.linalg_norm(a, ord, dim, keepdim, dtype=dtype).clone()
  )JIT";

  auto a = at::randn({2, 3});
  auto dim = std::vector<int64_t>({0, 1});
  auto dtype = at::ScalarType::Float;

  std::vector<IValue> args0{a, "fro", dim, true, dtype};
  testStaticRuntime(linalg_norm_ord_str, args0);

  auto b = at::randn({3, 2, 17});
  std::vector<IValue> args1{b, "fro", dim, true, dtype};
  testStaticRuntime(linalg_norm_ord_str, args0, args1);
}

TEST(StaticRuntime, Cat) {
  const std::string cat_script = R"IR(
    graph(%a: Tensor, %b: Tensor, %dim: int):
        %ten_list: Tensor[] = prim::ListConstruct(%a, %b)
        %1 : int = prim::Constant[value=0]()
        %2 : int = prim::Constant[value=1]()
        %3 : int = prim::Constant[value=1]()
        %ten_list2 : Tensor[] = aten::slice(%ten_list, %1, %2, %3)
        %ret: Tensor = aten::cat(%ten_list2, %dim)
        return (%ret)
  )IR";

  auto graph = std::make_shared<Graph>();
  std::unordered_map<std::string, Value*> vmap;
  parseIR(cat_script, graph.get(), vmap);
  torch::jit::StaticModule smodule(graph);
  ASSERT_TRUE(getNodeWithKind(smodule, "aten::cat"));

  auto a = at::randn({2, 4});
  auto b = at::randn({3, 4});
  std::vector<IValue> args0{a, b, 0};

  testStaticRuntime(cat_script, args0);

  auto c = at::randn({3, 4});
  auto d = at::randn({3, 5});
  std::vector<IValue> args1{c, d, 1};
  testStaticRuntime(cat_script, args0, args1);

  std::vector<IValue> args_dim_negative{c, d, -1};
  testStaticRuntime(cat_script, args_dim_negative);
}

TEST(StaticRuntime, Cumsum) {
  const auto cumsum_script = R"JIT(
    def forward(self, a: Tensor, dim: int):
        return torch.cumsum(a, dim).clone()
  )JIT";

  auto a = at::randn({2, 3});
  std::vector<IValue> args0{a, 0};
  testStaticRuntime(cumsum_script, args0);

  auto b = at::randn({3, 6});
  std::vector<IValue> args1{b, 1};
  testStaticRuntime(cumsum_script, args0, args1);
}

TEST(StaticRuntime, CumsumDtype) {
  const auto cumsum_script_dtype = R"JIT(
    def forward(self, a: Tensor, dim: int, dtype: int):
        return torch.cumsum(a, dim, dtype=dtype).clone()
  )JIT";

  auto a = at::randn({1, 2});
  auto dtype = at::ScalarType::Float;
  std::vector<IValue> args0{a, 0, dtype};
  testStaticRuntime(cumsum_script_dtype, args0);

  auto b = at::randn({3, 6});
  std::vector<IValue> args1{b, 1, dtype};
  testStaticRuntime(cumsum_script_dtype, args0, args1);
}

TEST(StaticRuntime, Nonzero) {
  const auto nonzero_tensor = R"JIT(
    def forward(self, input: Tensor):
        a = torch.nonzero(input).clone()
        return (a)
  )JIT";

  auto a = at::randint(0, 2, {2, 3});
  testStaticRuntime(nonzero_tensor, {a});

  auto b = at::randint(0, 2, {4, 3, 2});
  testStaticRuntime(nonzero_tensor, {a}, {b});
}

TEST(StaticRuntime, SignedLog1p) {
  const std::string signed_log1p_script = R"IR(
    graph(%input):
        %0 : Tensor = aten::sign(%input)
        %1 : Tensor = aten::abs(%input)
        %2 : Tensor = aten::log1p(%1)
        %3 : Tensor = aten::mul(%0, %2)
        %none : NoneType = prim::Constant()
        %res : Tensor = aten::clone(%3, %none)
        return (%res)
  )IR";

  std::vector<IValue> args1 = {at::randn({2, 2})};
  testStaticRuntime(signed_log1p_script, args1, {}, true);

  std::vector<IValue> args2 = {at::randn({3, 3, 3})};
  testStaticRuntime(signed_log1p_script, args1, args2, true);
}

TEST(StaticRuntime, RemoveImmutableInputDictLookupsWithImmutableInputDict) {
  const auto getitem_immutable_input_dict_script = R"JIT(
    def forward(self, input: Dict[int, Tensor]):
        a = input[0]
        b = input[1]
        c = a + b
        return c.clone()
  )JIT";

  script::Module module("module");
  module.define(getitem_immutable_input_dict_script);
  torch::jit::StaticModule smodule(module);
  EXPECT_FALSE(hasNodeWithKind(smodule, "aten::__getitem__"));
  EXPECT_TRUE(hasNodeWithKind(smodule, "static_runtime::dict_unpack"));

  auto a = at::randn({2, 4});
  auto b = at::randn({2, 4});
  c10::Dict<c10::IValue, c10::IValue> dict(
      c10::IntType::get(), c10::TensorType::get());
  dict.insert(0, a);
  dict.insert(1, b);
  testStaticRuntime(getitem_immutable_input_dict_script, {dict});

  c10::Dict<c10::IValue, c10::IValue> dict0(
      c10::IntType::get(), c10::TensorType::get());
  auto a0 = at::randn({3, 4});
  auto b0 = at::randn({3, 4});
  dict0.insert(0, a0);
  dict0.insert(1, b0);
  testStaticRuntime(getitem_immutable_input_dict_script, {dict0});
}

TEST(StaticRuntime, RemoveImmutableInputDictLookupsWithMutableInputDict) {
  const auto getitem_mutable_input_dict_script = R"JIT(
    def forward(self, input: Dict[int, Tensor]):
        a = input[0]
        input[1] = a
        b = input[1]
        c = a + b
        return c.clone()
  )JIT";

  script::Module module("module");
  module.define(getitem_mutable_input_dict_script);
  torch::jit::StaticModule smodule(module);
  EXPECT_TRUE(hasNodeWithKind(smodule, "aten::__getitem__"));
  EXPECT_FALSE(hasNodeWithKind(smodule, "static_runtime::dict_unpack"));
}

TEST(StaticRuntime, VarTupleUnpack) {
  const auto var_tuple_unpack_script = R"JIT(
    def forward(self, input_0: Tuple[Tensor, Tensor], input_1: Tuple[int, int]):
        a, b = input_0
        c, d = input_1
        res = a * c + b * d
        return res.clone()
  )JIT";

  script::Module module("module");
  module.define(var_tuple_unpack_script);
  torch::jit::StaticModule smodule(module);
  EXPECT_FALSE(hasNodeWithKind(smodule, "prim::TupleUnpack"));
  EXPECT_TRUE(hasNodeWithKind(smodule, "static_runtime::VarTupleUnpack"));

  auto a = at::randn({2, 2});
  auto b = at::randn({3, 3, 3});
  std::vector<IValue> args1{
      c10::ivalue::Tuple::create(a, a), c10::ivalue::Tuple::create(1, 2)};
  std::vector<IValue> args2{
      c10::ivalue::Tuple::create(b, b), c10::ivalue::Tuple::create(1, 2)};

  testStaticRuntime(var_tuple_unpack_script, args1);
  testStaticRuntime(var_tuple_unpack_script, args1, args2);
}

TEST(StaticRuntime, VarTupleUnpack_NotApplied) {
  const auto var_tuple_unpack_not_applied_script = R"JIT(
    def forward(self, input_0: Tuple[Tensor, Tensor], input_1: Tuple[int, int]):
        a, b = input_0
        x = a + b
        c, d = input_1
        res = a * c + b * d + x
        return res.clone()
  )JIT";

  script::Module module("module");
  // In this script, the optimization is not applied since there is a
  // computation between the TupleUnpack nodes.
  module.define(var_tuple_unpack_not_applied_script);
  torch::jit::StaticModule smodule(module);
  EXPECT_FALSE(hasNodeWithKind(smodule, "static_runtime::VarTupleUnpack"));
  EXPECT_TRUE(hasNodeWithKind(smodule, "prim::TupleUnpack"));
}

TEST(StaticRuntime, RemainderTensor) {
  const auto remainder_tensor = R"JIT(
    def forward(self, x, y):
        return torch.remainder(x, y).clone()
  )JIT";

  std::vector<IValue> args1 = {
      at::randint(0, 10, {2, 2}), at::randint(0, 10, {2, 2})};
  std::vector<IValue> args2 = {
      at::randint(0, 10, {3, 6}), at::randint(0, 10, {3, 6})};

  // Use allclose and equalnan since outputs may be NaN.
  testStaticRuntime(
      remainder_tensor,
      args1,
      /*args2*/ {},
      /*use_alloclose*/ true,
      /*use_equalnan*/ true);
  testStaticRuntime(
      remainder_tensor,
      args1,
      args2,
      /*use_allclose*/ true,
      /*use_equalnan*/ true);
}

TEST(StaticRuntime, RemainderScalar) {
  const auto remainder_scalar = R"JIT(
    def forward(self, x, y: int):
        return torch.remainder(x, y).clone()
  )JIT";

  std::vector<IValue> args1 = {at::randint(0, 10, {2, 2}), 4};
  std::vector<IValue> args2 = {at::randint(0, 10, {3, 6}), 4};

  // Use allclose and equalnan since outputs may be NaN.
  testStaticRuntime(
      remainder_scalar,
      args1,
      /*args2*/ {},
      /*use_alloclose*/ true,
      /*use_equalnan*/ true);
  testStaticRuntime(
      remainder_scalar,
      args1,
      args2,
      /*use_allclose*/ true,
      /*use_equalnan*/ true);
}

TEST(StaticRuntime, Where) {
  const auto where_script = R"JIT(
    def forward(self, x, y):
        return torch.where(x > 0, x, y).clone()
  )JIT";

  std::vector<IValue> args1_fallback = {at::randn({2, 2}), at::randn({2, 2})};
  std::vector<IValue> args2_fallback = {at::randn({3, 6}), at::randn({3, 6})};

  std::vector<IValue> args1_nnc = {
      at::randint(-10, 10, {2, 2}, at::kLong),
      at::randint(-10, 10, {2, 2}, at::kLong)};
  std::vector<IValue> args2_nnc = {
      at::randint(-10, 10, {3, 6}, at::kLong),
      at::randint(-10, 10, {3, 6}, at::kLong)};

  testStaticRuntime(where_script, args1_fallback);
  testStaticRuntime(where_script, args1_fallback, args2_fallback);

  testStaticRuntime(where_script, args1_nnc);
  testStaticRuntime(where_script, args1_nnc, args2_nnc);
}

TEST(StaticRuntime, View) {
  // Note that clone is not technically necessary here since this is not
  // an out variant, but it suppresses warnings about only have one op
  // in testStaticRuntime
  const auto src = R"IR(
    graph(%input : Tensor, %shape : int[]):
        %none : NoneType = prim::Constant()
        %view : Tensor = aten::view(%input, %shape)
        %res : Tensor = aten::clone(%view, %none)
        return (%res)
  )IR";

  std::vector<IValue> args1{at::randn({2, 2}), c10::List<int64_t>(4)};
  std::vector<IValue> args2{at::randn({2, 2, 2}), c10::List<int64_t>({4, 2})};

  testStaticRuntime(src, args1);
  testStaticRuntime(src, args1, args2);
}

TEST(StaticRuntime, Size) {
  const auto src = R"JIT(
      def forward(self, x, dim: int):
          return x.size(dim)
  )JIT";

  std::vector<IValue> args1{at::randn({1}), 0};
  std::vector<IValue> args2{at::randn({1}), -1};
  std::vector<IValue> args3{at::randn({2, 4}), 1};

  testStaticRuntime(src, args1);
  testStaticRuntime(src, args2);
  testStaticRuntime(src, args1, args3);
}

TEST(StaticRuntime, Squeeze) {
  // Note: this is a native op, not an out variant, but clone anyways
  // to silence warnings in testStaticRuntime
  const auto src = R"JIT(
    def forward(self, inp, dim: int):
        return inp.squeeze(dim).clone()
  )JIT";

  const auto a = at::randn({2, 2});
  const auto b = at::randn({3, 2, 3});

  testStaticRuntime(src, {a, 0});
  testStaticRuntime(src, {a, 1});
  testStaticRuntime(src, {a, -1}, {b, 2});
}

TEST(StaticRuntime, NumToTensorScalar) {
  const auto num_to_tensor_ir = R"IR(
    graph(%1 : int):
      %2 : NoneType = prim::Constant()
      %3 : Tensor = prim::NumToTensor(%1)
      %4 : Tensor = aten::clone(%3, %2)
      return (%4)
  )IR";

  IValue arg{5};
  std::vector<IValue> args = {arg};
  testStaticRuntime(num_to_tensor_ir, args);
}

TEST(StaticRuntime, NumToTensorFalse) {
  const auto num_to_tensor_ir = R"IR(
    graph(%1 : bool):
      %2 : NoneType = prim::Constant()
      %3 : Tensor = prim::NumToTensor(%1)
      %4 : Tensor = aten::clone(%3, %2)
      return (%4)
  )IR";

  IValue arg{false};
  std::vector<IValue> args = {arg};
  testStaticRuntime(num_to_tensor_ir, args);
}

TEST(StaticRuntime, NumToTensorTrue) {
  const auto num_to_tensor_ir = R"IR(
    graph(%1 : bool):
      %2 : NoneType = prim::Constant()
      %3 : Tensor = prim::NumToTensor(%1)
      %4 : Tensor = aten::clone(%3, %2)
      return (%4)
  )IR";

  IValue arg{true};
  std::vector<IValue> args = {arg};
  testStaticRuntime(num_to_tensor_ir, args);
}

TEST(StaticRuntime, Split) {
  const auto src = R"JIT(
    def forward(self, inp, split_size: int, dim: int):
        return inp.split(split_size, dim)
  )JIT";

  const auto a = at::randn({2, 2});
  const auto b = at::randn({2, 2, 2});

  testStaticRuntime(src, {a, 1, 0});
  testStaticRuntime(src, {a, 1, 1});
  testStaticRuntime(src, {a, 2, -1}, {b, 2, 2});
}

TEST(StaticRuntime, SplitWithSizes) {
  const auto src = R"JIT(
    def forward(self, inp, split_sizes: List[int], dim: int):
        return inp.split(split_sizes, dim)
  )JIT";

  const auto a = at::randn({2, 2});
  const auto b = at::randn({2, 2, 2});
  const auto split_sizes = c10::List<int64_t>{1, 1};

  testStaticRuntime(src, {a, split_sizes, 0});
  testStaticRuntime(src, {a, split_sizes, 1});
  testStaticRuntime(src, {a, split_sizes, -1}, {b, split_sizes, 2});
}

namespace {

void maybe_throw(bool should_throw) {
  if (should_throw) {
    throw std::runtime_error("test exception");
  }
}

TORCH_LIBRARY(static_runtime_tests, m) {
  // Conservative so this op doesn't get deleted by dead
  // code elimination
  m.def(torch::schema(
      "static_runtime_tests::maybe_throw(bool throw) -> ()",
      at::AliasAnalysisKind::CONSERVATIVE));
  m.impl("maybe_throw", maybe_throw);
}

} // namespace

TEST(StaticRuntime, ModelCrashOnFirstRun) {
  const auto src = R"JIT(
    graph(%0: Tensor, %throw: bool):
        %1: Tensor = aten::mul(%0, %0)
        static_runtime_tests::maybe_throw(%throw)
        %2: Tensor = aten::mul(%1, %1)
        %3: Tensor = aten::mul(%2, %2)
        return (%3)
  )JIT";

  auto graph = getGraphFromIR(src);
  auto static_module = StaticModule(graph);
  auto& runtime = static_module.runtime();

  std::vector<IValue> args_crash{at::randn({1}), true};
  std::vector<IValue> args_no_crash{at::randn({1}), false};
  EXPECT_THROW(runtime(args_crash, {}), std::runtime_error);

  // The run didn't finish, we didn't allocate the memory planner
  EXPECT_EQ(runtime.get_memory_planner(), nullptr);
  runtime.check_for_memory_leak();

  // We guarantee that the runtime is still usable after the crash.
  // Run again to verify this.
  compareResultsWithJIT(runtime, graph, args_no_crash);
  EXPECT_NE(runtime.get_memory_planner(), nullptr);
}

TEST(StaticRuntime, ModelCrashOnSecondRun) {
  const auto src = R"JIT(
    graph(%0: Tensor, %throw: bool):
        %1: Tensor = aten::mul(%0, %0)
        static_runtime_tests::maybe_throw(%throw)
        %2: Tensor = aten::mul(%1, %1)
        %3: Tensor = aten::mul(%2, %2)
        return (%3)
  )JIT";

  auto graph = getGraphFromIR(src);
  auto static_module = StaticModule(graph);
  auto& runtime = static_module.runtime();

  std::vector<IValue> args_crash{at::randn({1}), true};
  std::vector<IValue> args_no_crash{at::randn({1}), false};
  runtime(args_no_crash, {});
  EXPECT_NE(runtime.get_memory_planner(), nullptr);
  runtime.check_for_memory_leak();

  EXPECT_THROW(runtime(args_crash, {}), std::runtime_error);
  runtime.check_for_memory_leak();

  // We guarantee that the runtime is still usable after the crash.
  // Run again to verify this.
  compareResultsWithJIT(runtime, graph, args_no_crash);
}
