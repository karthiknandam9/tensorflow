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

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/compiler/xla/tests/test_macros.h"
#include "tensorflow/compiler/xla/tests/test_utils.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/blocking_counter.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/threadpool.h"

// Tests cross-GPU operations.
//
// Several tests requires at least four GPUs.  For instructions on running this
// within Google, see go/multi-gpu-unit-test.

#define SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(x)                     \
  if (num_devices_ < x) {                                         \
    GTEST_SKIP() << "Test requires at least " << x << " devices"; \
  }

namespace xla {
namespace {

class CollectiveOpsTest : public HloTestBase {
 public:
  CollectiveOpsTest() : num_devices_(backend().device_count()) {
    VLOG(1) << "Running with " << num_devices_ << " devices";
  }

 protected:
  DebugOptions GetDebugOptionsForTest() override {
    DebugOptions debug_options = HloTestBase::GetDebugOptionsForTest();
    // Disable async->sync collective conversion pass to enable unit testing
    // of async collectives.
    debug_options.add_xla_disable_hlo_passes(
        "gpu-convert-async-collectives-to-sync");
    return debug_options;
  }

  std::unique_ptr<HloModule> MakeCrsModule(
      const Shape& shape, std::vector<std::vector<int64_t>> replica_groups,
      const HloModuleConfig& config, std::string op = "add",
      std::string datatype = "f32") {
    std::string hlo_template = R"(
      HloModule test

      apply_op {
        x = DATATYPE[] parameter(0)
        y = DATATYPE[] parameter(1)
        ROOT apply_op = DATATYPE[] OP(x, y)
      }

      ENTRY test_computation {
        p = SHAPE parameter(0)
        p2 = SHAPE reshape(p)
        crs = SHAPE all-reduce(p2), replica_groups=REPLICA_GROUPS, to_apply=apply_op
        copy = SHAPE copy(crs)
        ROOT out = SHAPE reshape(copy)
      }
    )";
    std::vector<std::string> replica_group_strs;
    replica_group_strs.reserve(replica_groups.size());
    for (const auto& g : replica_groups) {
      replica_group_strs.push_back(
          absl::StrFormat("{%s}", absl::StrJoin(g, ",")));
    }
    std::string shape_str = shape.ToString(/*print_layout=*/false);
    if (shape_str == "f32[1]") {
      // Exercise the scalar codepath.
      hlo_template = absl::StrReplaceAll(
          hlo_template,
          {{"DATATYPE[SHAPE] reshape(p)", "DATATYPE[] reshape(p)"},
           {"DATATYPE[SHAPE] all-reduce", "DATATYPE[] all-reduce"},
           {"DATATYPE[SHAPE] copy", "DATATYPE[] copy"}});
    }
    std::string parameterized_hlo = absl::StrReplaceAll(
        hlo_template,
        {{"SHAPE", shape_str},
         {"REPLICA_GROUPS",
          absl::StrFormat("{%s}", absl::StrJoin(replica_group_strs, ", "))},
         {"OP", op},
         {"DATATYPE", datatype}});
    return ParseAndReturnVerifiedModule(parameterized_hlo, config).value();
  }

  template <typename LiteralType>
  void TestTwoReplicasOneOperand(std::string op, Literal input_value,
                                 Literal expected_value) {
    const int kNumReplicas = 2;
    std::string dtype = primitive_util::LowercasePrimitiveTypeName(
        primitive_util::NativeToPrimitiveType<LiteralType>());
    HloModuleConfig config =
        GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
    auto module = MakeCrsModule(
        /*shape=*/input_value.shape(),
        /*replica_groups=*/{}, config,
        /*op=*/op, /*datatype=*/dtype);
    TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                            ExecuteReplicated(std::move(module), {&input_value},
                                              /*num_replicas=*/kNumReplicas,
                                              /*use_threads=*/true,
                                              /*run_hlo_passes=*/true));
    for (int replica_idx = 0; replica_idx < kNumReplicas; replica_idx++) {
      EXPECT_TRUE(LiteralTestUtil::NearOrEqual(
          expected_value, results[replica_idx], ErrorSpec{1e-5, 1e-5}));
    }
  }

  template <typename LiteralType>
  void TestAllOpsForReduce() {
    auto cast = [&](int value) { return static_cast<LiteralType>(value); };
    auto to_literal = [&](absl::Span<const LiteralType> values) {
      return LiteralUtil::CreateR1<LiteralType>(values);
    };
    Literal input_value = to_literal({cast(1), cast(2), cast(3)});
    TestTwoReplicasOneOperand<LiteralType>(
        "add",
        /*input_value=*/input_value.Clone(),
        /*expected_value=*/to_literal({cast(2), cast(4), cast(6)}));
    TestTwoReplicasOneOperand<LiteralType>(
        "multiply",
        /*input_value=*/input_value.Clone(),
        /*expected_value=*/to_literal({cast(1), cast(4), cast(9)}));
    TestTwoReplicasOneOperand<LiteralType>(
        "maximum",
        /*input_value=*/input_value.Clone(),
        /*expected_value=*/to_literal({cast(1), cast(2), cast(3)}));
    TestTwoReplicasOneOperand<LiteralType>(
        "minimum",
        /*input_value=*/input_value.Clone(),
        /*expected_value=*/to_literal({cast(1), cast(2), cast(3)}));
  }

 protected:
  const int64_t num_devices_;
};

// Returns the non-empty subsets of {0, 1, ..., n}.  For example,
// PowerSetOfIota(3) = {{0}, {1}, {2}, {0,1}, {0,2}, {1,2}, {0,1,2}}.
std::vector<std::vector<int64_t>> PowerSetOfIota(int64_t n) {
  std::vector<std::vector<int64_t>> power_set;
  for (int64_t i = 1; i < (1 << n); ++i) {
    power_set.emplace_back();
    for (int64_t j = 0; j < n; ++j) {
      if (i & (1 << j)) {
        power_set.back().push_back(j);
      }
    }
  }
  return power_set;
}

// Makes a DeviceAssignment assigning replica-id i to devices[i].
DeviceAssignment MakeDeviceAssn(std::vector<int64_t> devices) {
  DeviceAssignment assn(/*replica_count=*/devices.size(),
                        /*computation_count=*/1);
  for (int64_t i = 0; i < devices.size(); ++i) {
    assn(i, 0) = devices[i];
  }
  return assn;
}

template <typename T>
static Eigen::half ToHalf(T value) {
  return static_cast<Eigen::half>(value);
}

XLA_TEST_F(CollectiveOpsTest, AllReduce_sum_float32_2D) {
  TestTwoReplicasOneOperand<float>(
      "add",
      /*input_value=*/LiteralUtil::CreateR2<float>({{1, 2}, {3, 4}}),
      /*expected_value=*/LiteralUtil::CreateR2<float>({{2, 4}, {6, 8}}));
}

XLA_TEST_F(CollectiveOpsTest, AllReduceSingleOutput_float32) {
  TestTwoReplicasOneOperand<float>(
      "add",
      /*input_value=*/LiteralUtil::CreateR1<float>({1}),
      /*expected_value=*/LiteralUtil::CreateR1<float>({2}));
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_int8) {
  TestAllOpsForReduce<int8_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_uint8) {
  TestAllOpsForReduce<uint8_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_uint32) {
  TestAllOpsForReduce<uint32_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_int32) {
  TestAllOpsForReduce<int32_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_int64) {
  TestAllOpsForReduce<int64_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_uint64) {
  TestAllOpsForReduce<uint64_t>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_float32) {
  TestAllOpsForReduce<float>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_double) {
  TestAllOpsForReduce<double>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_half) {
  TestAllOpsForReduce<Eigen::half>();
}

XLA_TEST_F(CollectiveOpsTest, AllReduceTwoReplicasOneOperand_bfloat16) {
  TestAllOpsForReduce<bfloat16>();
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AllReduce_sum_complex64)) {
  TestTwoReplicasOneOperand<complex64>(
      "add",
      /*input_value=*/LiteralUtil::CreateR1<complex64>({{1, 2}, {3, 4}}),
      /*expected_value=*/LiteralUtil::CreateR1<complex64>({{2, 4}, {6, 8}}));
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AllReduce_sum_complex128)) {
  TestTwoReplicasOneOperand<complex128>(
      "add",
      /*input_value=*/LiteralUtil::CreateR1<complex128>({{1, 2}, {3, 4}}),
      /*expected_value=*/LiteralUtil::CreateR1<complex128>({{2, 4}, {6, 8}}));
}

XLA_TEST_F(CollectiveOpsTest, AllReduceAnd_Pred) {
  // Test with equal elements.
  TestTwoReplicasOneOperand<bool>(
      "and",
      /*input_value=*/LiteralUtil::CreateR1<bool>({true, false}),
      /*expected_value=*/LiteralUtil::CreateR1<bool>({true, false}));

  // Test with {true, false}.
  const char* hlo_module = R"(
    HloModule test

    apply_op {
      x = pred[] parameter(0)
      y = pred[] parameter(1)
      ROOT apply_op = pred[] and(x, y)
    }

    ENTRY test_computation {
      id = u32[] replica-id()
      c = u32[] constant(0)
      p = pred[] compare(id, c), direction=EQ
      p2 = pred[1] reshape(p)
      crs = pred[1] all-reduce(p2), replica_groups={}, to_apply=apply_op
      copy = pred[1] copy(crs)
      ROOT out = pred[1] reshape(copy)
    }
  )";

  HloModuleConfig config = GetModuleConfigForTest(/*replica_count=*/2);
  auto module = ParseAndReturnVerifiedModule(hlo_module, config).value();
  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {},
                        /*num_replicas=*/2,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  for (int replica_idx = 0; replica_idx < 2; replica_idx++) {
    EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<bool>({false}),
                                       results[replica_idx]));
  }
}

XLA_TEST_F(CollectiveOpsTest, AllReduceOr_Pred) {
  // Test with equal elements.
  TestTwoReplicasOneOperand<bool>(
      "or",
      /*input_value=*/LiteralUtil::CreateR1<bool>({true, false}),
      /*expected_value=*/LiteralUtil::CreateR1<bool>({true, false}));

  // Test with {true, false}.
  const char* hlo_module = R"(
    HloModule test

    apply_op {
      x = pred[] parameter(0)
      y = pred[] parameter(1)
      ROOT apply_op = pred[] or(x, y)
    }

    ENTRY test_computation {
      id = u32[] replica-id()
      c = u32[] constant(0)
      p = pred[] compare(id, c), direction=EQ
      p2 = pred[1] reshape(p)
      crs = pred[1] all-reduce(p2), replica_groups={}, to_apply=apply_op
      copy = pred[1] copy(crs)
      ROOT out = pred[1] reshape(copy)
    }
  )";

  HloModuleConfig config = GetModuleConfigForTest(/*replica_count=*/2);
  auto module = ParseAndReturnVerifiedModule(hlo_module, config).value();
  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {},
                        /*num_replicas=*/2,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  for (int replica_idx = 0; replica_idx < 2; replica_idx++) {
    EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<bool>({true}),
                                       results[replica_idx]));
  }
}

// Tries all-reduce operations across all 2^kNumReplicas - 1 combinations of
// devices in sequence.
XLA_TEST_F(CollectiveOpsTest, AllReduce_AllCombinations) {
  const int64_t kNumElems = 1024;

  for (std::vector<int64_t> devices : PowerSetOfIota(num_devices_)) {
    SCOPED_TRACE(absl::StrFormat("Running on devices {%s}",
                                 absl::StrJoin(devices, ", ")));

    DeviceAssignment device_assn = MakeDeviceAssn(devices);

    HloModuleConfig config =
        GetModuleConfigForTest(/*replica_count=*/devices.size());
    config.set_static_device_assignment(device_assn);

    std::vector<float> input_vec(kNumElems);
    absl::c_iota(input_vec, 0);
    auto input_literal = LiteralUtil::CreateR1<float>(input_vec);

    auto module = MakeCrsModule(input_literal.shape(),
                                /*replica_groups=*/{}, config);

    TF_ASSERT_OK_AND_ASSIGN(
        std::vector<Literal> results,
        ExecuteReplicated(std::move(module), {&input_literal},
                          /*num_replicas=*/devices.size(), &device_assn,
                          /*run_hlo_passes=*/true, /*use_threads=*/true));
  }
}

// Runs the same executable many times concurrently.  The all-reduces should not
// conflict with one another.
// http://b/259130904 [XLA:GPU] AllReduce_ManyConcurrentAllReduces subtest fails
//                     with async all-reduce enables
XLA_TEST_F(CollectiveOpsTest,
           DISABLED_ON_GPU(AllReduce_ManyConcurrentAllReduces)) {
  const int64_t kNumElems = 1024;
  const int64_t kNumThreads = 200;
  const int64_t kRunsPerThread = 10;

  std::vector<float> input_vec(kNumElems);
  absl::c_iota(input_vec, 0);
  auto input_literal = LiteralUtil::CreateR1<float>(input_vec);

  HloModuleConfig config = GetModuleConfigForTest(/*replica_count=*/2);
  auto executable =
      test_runner_
          .CreateExecutable(MakeCrsModule(input_literal.shape(),
                                          /*replica_groups=*/{}, config),
                            /*run_hlo_passes=*/true)
          .value();
  std::vector<int64_t> devices = {0, 1};
  auto device_assn = MakeDeviceAssn(devices);

  HloRunner::ReplicatedExecuteOptions opts;
  opts.num_replicas = devices.size();
  opts.use_threads = true;
  opts.arguments.push_back(&input_literal);

  tsl::BlockingCounter done(kNumThreads * kRunsPerThread);
  tsl::thread::ThreadPool pool(tsl::Env::Default(), TestName(), kNumThreads);
  for (int64_t i = 0; i < kNumThreads * kRunsPerThread; ++i) {
    pool.Schedule([&] {
      TF_ASSERT_OK(
          test_runner_.ExecuteReplicated(executable.get(), opts, &device_assn)
              .status());
      done.DecrementCount();
    });
  }
  done.Wait();
}

// Runs the same executable many times concurrently.  The all-reduces should not
// conflict with one another.
XLA_TEST_F(CollectiveOpsTest, AllReduce_CombinableAllReduces) {
  std::string hlo_string = R"(
    HloModule test

    apply_op {
      x = f32[] parameter(0)
      y = f32[] parameter(1)
      ROOT apply_op = f32[] add(x, y)
    }

    ENTRY test_computation {
      p0 = f32[5] parameter(0)
      p1 = f32[5] parameter(1)
      crs0 = f32[5] all-reduce(p0), replica_groups={}, to_apply=apply_op
      crs1 = f32[5] all-reduce(p1), replica_groups={}, to_apply=apply_op
      ROOT out = (f32[5], f32[5]) tuple(f32[5] crs0, f32[5] crs1)
    }
  )";
  static constexpr int kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string, config));

  std::vector<float> input0_vec = {1., 2., 3., 4., 5.};
  auto input0_literal = LiteralUtil::CreateR1<float>(input0_vec);
  std::vector<float> input1_vec = {7., 3., 4., 1., 2.};
  auto input1_literal = LiteralUtil::CreateR1<float>(input1_vec);

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {&input0_literal, &input1_literal},
                        /*num_replicas=*/kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  std::vector<float> expected0_vec = {2., 4., 6., 8., 10.};
  auto expected0_literal = LiteralUtil::CreateR1<float>(expected0_vec);
  std::vector<float> expected1_vec = {14., 6., 8., 2., 4.};
  auto expected1_literal = LiteralUtil::CreateR1<float>(expected1_vec);
  for (int replica_idx = 0; replica_idx < kNumReplicas; replica_idx++) {
    auto rs = results[replica_idx].DecomposeTuple();
    EXPECT_TRUE(LiteralTestUtil::NearOrEqual(expected0_literal, rs[0],
                                             ErrorSpec{1e-5, 1e-5}));
    EXPECT_TRUE(LiteralTestUtil::NearOrEqual(expected1_literal, rs[1],
                                             ErrorSpec{1e-5, 1e-5}));
  }
}

// Runs an all-reduce with three partitions:
//  {0}, {1,2}, {3}
// meaning, the all-reduce is a nop for devices 0 and 3, and only devices 1 and
// 2 actually exchange data with each other.
XLA_TEST_F(CollectiveOpsTest, AllReduce_ThreeReplicaGroups) {
  // Test a prime number so it's not all powers of 2.
  const int64_t kNumElems = 137;
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  std::vector<float> input_vec(kNumElems);
  absl::c_iota(input_vec, 0);
  auto input_literal = LiteralUtil::CreateR1<float>(input_vec);
  auto module = MakeCrsModule(
      /*shape=*/input_literal.shape(),
      /*replica_groups=*/{{0}, {1, 2}, {3}}, config);

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {&input_literal}, /*num_replicas=*/4,
                        /*use_threads=*/true));

  ASSERT_EQ(results.size(), 4);

  std::vector<float> input_vec_doubled;
  input_vec_doubled.reserve(input_vec.size());
  for (float n : input_vec) {
    input_vec_doubled.push_back(n * 2);
  }
  auto input_literal_doubled = LiteralUtil::CreateR1<float>(input_vec_doubled);

  EXPECT_TRUE(LiteralTestUtil::Equal(input_literal, results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(input_literal_doubled, results[1]));
  EXPECT_TRUE(LiteralTestUtil::Equal(input_literal_doubled, results[2]));
  EXPECT_TRUE(LiteralTestUtil::Equal(input_literal, results[3]));
}

XLA_TEST_F(CollectiveOpsTest, AllReduce_Degenerate) {
  const char* const kModuleStr = R"(
      HloModule test

      apply_op {
        x = u32[] parameter(0)
        y = u32[] parameter(1)
        ROOT apply_op = u32[] add(x, y)
      }

      ENTRY test_computation {
        id = u32[] replica-id()
        ROOT crs = u32[] all-reduce(id), replica_groups={{0},{1},{2},{3}}, to_apply=apply_op
      }
    )";
  static constexpr int kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));
  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, /*num_replicas=*/kNumReplicas,
                        /*use_threads=*/true));

  ASSERT_EQ(results.size(), kNumReplicas);
  for (int i = 0; i < kNumReplicas; ++i) {
    LiteralTestUtil::ExpectR0Equal<uint32_t>(i, results[i]);
  }
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncAllReduce)) {
  const absl::string_view kModuleStr = R"(
      HloModule test

      apply_op {
        x = u32[] parameter(0)
        y = u32[] parameter(1)
        ROOT apply_op = u32[] add(x, y)
      }

      ENTRY test_computation {
        id = u32[] replica-id()
        start = u32[] all-reduce-start(id), to_apply=apply_op, backend_config="{\"is_sync\":false}"
        ROOT done = u32[] all-reduce-done(start)
      }
    )";

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/num_devices_);
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));
  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, num_devices_,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));

  ASSERT_EQ(results.size(), num_devices_);
  // sum [0, num_devices)
  uint32_t expected = num_devices_ * (num_devices_ - 1) / 2;
  for (int i = 0; i < num_devices_; ++i) {
    LiteralTestUtil::ExpectR0Equal<uint32_t>(expected, results[i]);
  }
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncAllReduceTwoOperands)) {
  const absl::string_view kModuleStr = R"(
      HloModule test

      apply_op {
        x = u32[] parameter(0)
        y = u32[] parameter(1)
        ROOT apply_op = u32[] add(x, y)
      }

      ENTRY test_computation {
        id = u32[] replica-id()
        id2 = u32[] multiply(id, id)
        start = (u32[], u32[]) all-reduce-start(id, id2), to_apply=apply_op, backend_config="{\"is_sync\":false}"
        ROOT done = (u32[], u32[]) all-reduce-done(start)
      }
    )";

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/num_devices_);
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));
  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, num_devices_,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));

  ASSERT_EQ(results.size(), num_devices_);
  // sum [0, num_devices)
  uint32_t expected0 = num_devices_ * (num_devices_ - 1) / 2;
  // sum squares [0, num_devices)
  uint32_t expected1 =
      num_devices_ * (num_devices_ - 1) * (2 * num_devices_ - 1) / 6;
  for (int i = 0; i < num_devices_; ++i) {
    std::vector<Literal> replica_results = results[i].DecomposeTuple();
    LiteralTestUtil::ExpectR0Equal<uint32_t>(expected0, replica_results[0]);
    LiteralTestUtil::ExpectR0Equal<uint32_t>(expected1, replica_results[1]);
  }
}

XLA_TEST_F(CollectiveOpsTest, ReplicaId) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    ROOT out = u32[] copy(id)
  }
  )";

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/num_devices_);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, num_devices_,
                                            /*use_threads=*/true));

  ASSERT_EQ(results.size(), num_devices_);
  for (uint32_t i = 0; i < num_devices_; ++i) {
    EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR0(i), results[i]));
  }
}

XLA_TEST_F(CollectiveOpsTest, CollectivePermute_Simple) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    permute = u32[2] collective-permute(p), source_target_pairs={{1,0}, {0,1}, {2,2}}
    ROOT copy = u32[2] copy(permute)
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({11, 11}),
                                     results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({10, 10}),
                                     results[1]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({12, 12}),
                                     results[2]));
  // Nothing writes to replica 3, so it is memzero'ed.
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({0, 0}),
                                     results[3]));
}

XLA_TEST_F(CollectiveOpsTest, CollectivePermute_Degenerate) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    permute = u32[2] collective-permute(p), source_target_pairs={{0,0}, {1,1}, {2,2}, {3,3}}
    ROOT copy = u32[2] copy(permute)
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({10, 10}),
                                     results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({11, 11}),
                                     results[1]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({12, 12}),
                                     results[2]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({13, 13}),
                                     results[3]));
}

XLA_TEST_F(CollectiveOpsTest, CollectivePermute_NotDegenerate) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    permute = u32[2] collective-permute(p), source_target_pairs={{0,0}, {1,1}, {2,2}}
    ROOT copy = u32[2] copy(permute)
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({10, 10}),
                                     results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({11, 11}),
                                     results[1]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({12, 12}),
                                     results[2]));
  // Nothing writes to replica 3, so it is memzero'ed.
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({0, 0}),
                                     results[3]));
}

XLA_TEST_F(CollectiveOpsTest, CollectivePermute_Rotate) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    replica = u32[] replica-id()
    ten = u32[] constant(10)
    sum = u32[] add(replica, ten)
    p = u32[2] broadcast(sum), dimensions={}
    permute = u32[2] collective-permute(p), source_target_pairs={{0,1}, {1,2}, {2,3}, {3,0}}
    ROOT copy = u32[2] copy(permute)
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({13, 13}),
                                     results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({10, 10}),
                                     results[1]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({11, 11}),
                                     results[2]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({12, 12}),
                                     results[3]));
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncCollectivePermute)) {
  const absl::string_view kModuleStr = R"(
      HloModule test

      ENTRY test_computation {
        replica = u32[] replica-id()
        ten = u32[] constant(10)
        sum = u32[] add(replica, ten)
        p = u32[2] broadcast(sum), dimensions={}
        start = (u32[2], u32[2]) collective-permute-start(p), source_target_pairs={{0,1}, {1,0}}, backend_config="{\"is_sync\":false}"
        ROOT done = u32[2] collective-permute-done(start)
      }
    )";

  const int64_t kNumReplicas = 2;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));
  ASSERT_EQ(results.size(), kNumReplicas);
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({11, 11}),
                                     results[0]));
  EXPECT_TRUE(LiteralTestUtil::Equal(LiteralUtil::CreateR1<uint32_t>({10, 10}),
                                     results[1]));
}

XLA_TEST_F(CollectiveOpsTest, AllToAll_EmptyReplicaGroups) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    b0 = u32[2] constant({20, 25})
    c0 = u32[2] constant({30, 35})
    d0 = u32[2] constant({40, 45})
    a1 = u32[2] add(id2, a0)
    b1 = u32[2] add(id2, b0)
    c1 = u32[2] add(id2, c0)
    d1 = u32[2] add(id2, d0)
    all2all = (u32[2], u32[2], u32[2], u32[2]) all-to-all(a1, b1, c1, d1), replica_groups={}
    a_prime = u32[2] get-tuple-element(all2all), index=0
    b_prime = u32[2] get-tuple-element(all2all), index=1
    c_prime = u32[2] get-tuple-element(all2all), index=2
    d_prime = u32[2] get-tuple-element(all2all), index=3
    ROOT out = u32[8] concatenate(a_prime, b_prime, c_prime, d_prime), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16, 12, 17, 13, 18},
                                           results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({20, 25, 21, 26, 22, 27, 23, 28},
                                           results[1]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({30, 35, 31, 36, 32, 37, 33, 38},
                                           results[2]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({40, 45, 41, 46, 42, 47, 43, 48},
                                           results[3]);
}

XLA_TEST_F(CollectiveOpsTest, AllToAll_OrderedReplicaGroups) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    b0 = u32[2] constant({20, 25})
    c0 = u32[2] constant({30, 35})
    d0 = u32[2] constant({40, 45})
    a1 = u32[2] add(id2, a0)
    b1 = u32[2] add(id2, b0)
    c1 = u32[2] add(id2, c0)
    d1 = u32[2] add(id2, d0)
    all2all = (u32[2], u32[2], u32[2], u32[2]) all-to-all(a1, b1, c1, d1), replica_groups={{3,2,1,0}}
    a_prime = u32[2] get-tuple-element(all2all), index=0
    b_prime = u32[2] get-tuple-element(all2all), index=1
    c_prime = u32[2] get-tuple-element(all2all), index=2
    d_prime = u32[2] get-tuple-element(all2all), index=3
    ROOT out = u32[8] concatenate(a_prime, b_prime, c_prime, d_prime), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({43, 48, 42, 47, 41, 46, 40, 45},
                                           results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({33, 38, 32, 37, 31, 36, 30, 35},
                                           results[1]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({23, 28, 22, 27, 21, 26, 20, 25},
                                           results[2]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({13, 18, 12, 17, 11, 16, 10, 15},
                                           results[3]);
}

XLA_TEST_F(CollectiveOpsTest, AllToAll_TwoReplicaGroups) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    b0 = u32[2] constant({20, 25})
    a1 = u32[2] add(id2, a0)
    b1 = u32[2] add(id2, b0)
    all2all = (u32[2], u32[2]) all-to-all(a1, b1), replica_groups={{2,1},{3,0}}
    a_prime = u32[2] get-tuple-element(all2all), index=0
    b_prime = u32[2] get-tuple-element(all2all), index=1
    ROOT out = u32[4] concatenate(a_prime, b_prime), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({23, 28, 20, 25}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({22, 27, 21, 26}, results[1]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({12, 17, 11, 16}, results[2]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({13, 18, 10, 15}, results[3]);
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AllToAll_SplitDimension)) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[4, 2] broadcast(id), dimensions={}
    a0 = u32[4, 2] constant({{10, 15}, {20, 25}, {30, 35}, {40, 45}})
    a1 = u32[4, 2] add(id2, a0)
    all2all = u32[4, 2] all-to-all(a1), replica_groups={{0,1,2,3}}, dimensions={0}
    ROOT out = u32[8] reshape(all2all)
  }
  )";
  const int64_t kNumReplicas = 4;
  SKIP_TEST_IF_NUM_DEVICES_LESS_THAN(kNumReplicas)

  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(std::vector<Literal> results,
                          ExecuteReplicated(std::move(module), {}, kNumReplicas,
                                            /*use_threads=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16, 12, 17, 13, 18},
                                           results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({20, 25, 21, 26, 22, 27, 23, 28},
                                           results[1]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({30, 35, 31, 36, 32, 37, 33, 38},
                                           results[2]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({40, 45, 41, 46, 42, 47, 43, 48},
                                           results[3]);
}

XLA_TEST_F(CollectiveOpsTest, AllGather_Dim0) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[1, 2] broadcast(id), dimensions={}
    a0 = u32[1, 2] constant({{10, 15}})
    a1 = u32[1, 2] add(id2, a0)
    allgather = u32[2, 2] all-gather(a1), dimensions={0}
    ROOT out = u32[4] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, AllGather_Dim1) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2, 1] broadcast(id), dimensions={}
    a0 = u32[2, 1] constant({{10}, {15}})
    a1 = u32[2, 1] add(id2, a0)
    allgather = u32[2, 2] all-gather(a1), dimensions={1}
    ROOT out = u32[4] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 11, 15, 16}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, AllReduce_TupleAllReduce) {
  if (IsMlirLoweringEnabled()) {
    // TupleAllReduce is not supported by MHLO. As of late 2022, there is no
    // known way to generate it from any frontend.
    GTEST_SKIP();
  }

  std::string hlo_string = R"(
    HloModule test

    apply_op {
      x = f32[] parameter(0)
      y = f32[] parameter(1)
      ROOT apply_op = f32[] add(x, y)
    }

    ENTRY test_computation {
      p0 = f32[5] parameter(0)
      p1 = f32[7] parameter(1)
      ROOT out = (f32[5], f32[7]) all-reduce(p0, p1), replica_groups={}, to_apply=apply_op
    }
  )";
  static constexpr int kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string, config));

  std::vector<float> input0_vec = {1., 2., 3., 4., 5.};
  auto input0_literal = LiteralUtil::CreateR1<float>(input0_vec);
  std::vector<float> input1_vec = {
      7., 3., 4., 1., 2., 3., 4.,
  };
  auto input1_literal = LiteralUtil::CreateR1<float>(input1_vec);

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {&input0_literal, &input1_literal},
                        /*num_replicas=*/kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  std::vector<float> expected0_vec = {2., 4., 6., 8., 10.};
  auto expected0_literal = LiteralUtil::CreateR1<float>(expected0_vec);
  std::vector<float> expected1_vec = {14., 6., 8., 2., 4., 6., 8.};
  auto expected1_literal = LiteralUtil::CreateR1<float>(expected1_vec);
  for (int replica_idx = 0; replica_idx < kNumReplicas; replica_idx++) {
    auto rs = results[replica_idx].DecomposeTuple();
    EXPECT_TRUE(LiteralTestUtil::NearOrEqual(expected0_literal, rs[0],
                                             ErrorSpec{1e-5, 1e-5}));
    EXPECT_TRUE(LiteralTestUtil::NearOrEqual(expected1_literal, rs[1],
                                             ErrorSpec{1e-5, 1e-5}));
  }
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AllGatherMixedTypes)) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    p0 = u32[2, 1] broadcast(id), dimensions={}
    p1 = f32[2, 1] convert(p0)
    allgather = (u32[2, 2], f32[2, 2]) all-gather(p0, p1), dimensions={1}
    ag0 = u32[2, 2] get-tuple-element(allgather), index=0
    ag1 = f32[2, 2] get-tuple-element(allgather), index=1
    r0 = u32[4] reshape(ag0)
    r1 = f32[4] reshape(ag1)
    ROOT out = (u32[4], f32[4]) tuple(r0, r1)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  for (int replica_idx = 0; replica_idx < kNumReplicas; replica_idx++) {
    auto rs = results[replica_idx].DecomposeTuple();
    LiteralTestUtil::ExpectR1Equal<uint32_t>({0, 1, 0, 1}, rs[0]);
    LiteralTestUtil::ExpectR1Near<float>({0.0, 1.0, 0.0, 1.0}, rs[1],
                                         ErrorSpec{1e-5, 1e-5});
  }
}

XLA_TEST_F(CollectiveOpsTest, ReduceScatter) {
  const char* const kModuleStr = R"(
  HloModule test
  add {
    lhs = u32[] parameter(0)
    rhs = u32[] parameter(1)
    ROOT add = u32[] add(lhs, rhs)
  }

  ENTRY main {
    c0 = u32[8] constant({1, 2, 3, 4, 5, 6, 7, 8})
    c1 = u32[8] constant({10, 11, 12, 13, 14, 15, 16, 17})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[8] select(pb, c0, c1)
    ROOT ars = u32[4] reduce-scatter(data), replica_groups={},
                      dimensions={0}, to_apply=add
  }
  )";

  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 13, 15, 17}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({19, 21, 23, 25}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, ReduceScatter_Dim1) {
  const char* const kModuleStr = R"(
  HloModule test
  add {
    lhs = u32[] parameter(0)
    rhs = u32[] parameter(1)
    ROOT add = u32[] add(lhs, rhs)
  }

  ENTRY main {
    c0 = u32[2, 4] constant({{ 1,  2,  3,  4}, { 5,  6,  7,  8}})
    c1 = u32[2, 4] constant({{10, 11, 12, 13}, {14, 15, 16, 17}})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[2, 4] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[2, 4] select(pb, c0, c1)
    // all-reduce result = {{11, 13, 15, 17}, {19, 21, 23, 25}}
    ars = u32[2, 2] reduce-scatter(data), replica_groups={},
                    dimensions={1}, to_apply=add
    ROOT r = u32[4] reshape(ars)
  }
  )";

  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 13, 19, 21}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({15, 17, 23, 25}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(ReduceScatterReassociate)) {
  const char* const kModuleStr = R"(
  HloModule m
  sum {
    a = u32[] parameter(0)
    b = u32[] parameter(1)
    ROOT add.2 = u32[] add(a, b)
  }

  ENTRY main {
    c0 = u32[8] constant({  1,  2,  3,  4,  5,  6,  7,  8})
    c1 = u32[8] constant({ 11, 12, 13, 14, 15, 16, 17, 18})
    c2 = u32[8] constant({  2,  3,  4,  5,  6,  7,  8,  9})
    c3 = u32[8] constant({ 12, 13, 14, 15, 16, 17, 18, 19})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data0 = c0 for replica 0 and c1 for replica 1
    data0 = u32[8] select(pb, c0, c1)
    // data1 = c2 for replica 0 and c3 for replica 1
    data1 = u32[8] select(pb, c2, c3)

    rs0 = u32[4] reduce-scatter(data0), replica_groups={}, dimensions={0}, to_apply=sum
    rs1 = u32[4] reduce-scatter(data1), replica_groups={}, dimensions={0}, to_apply=sum
    ROOT add = u32[4] add(rs0, rs1)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));

  const ErrorSpec es{1e-5, 1e-5};
  LiteralTestUtil::ExpectR1Equal<uint32_t>({26, 30, 34, 38}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({42, 46, 50, 54}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest,
           DISABLED_ON_CPU(ReduceScatterReassociate_ReduceScatterCreator)) {
  const char* const kModuleStr = R"(
  HloModule m
  sum {
    a = u32[] parameter(0)
    b = u32[] parameter(1)
    ROOT add.2 = u32[] add(a, b)
  }

  ENTRY main {
    c0 = u32[8] constant({  1,  2,  3,  4,  5,  6,  7,  8})
    c1 = u32[8] constant({ 11, 12, 13, 14, 15, 16, 17, 18})
    c2 = u32[8] constant({  2,  3,  4,  5,  6,  7,  8,  9})
    c3 = u32[8] constant({ 12, 13, 14, 15, 16, 17, 18, 19})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data0 = c0 for replica 0 and c1 for replica 1
    data0 = u32[8] select(pb, c0, c1)
    // data1 = c2 for replica 0 and c3 for replica 1
    data1 = u32[8] select(pb, c2, c3)

    ar0 = u32[8] all-reduce(data0), replica_groups={}, to_apply=sum
    ar1 = u32[8] all-reduce(data1), replica_groups={}, to_apply=sum
    rid = u32[] replica-id()
    slice_size = u32[] constant(4)
    offset = u32[] multiply(rid, slice_size)
    ds0 = u32[4] dynamic-slice(ar0, offset), dynamic_slice_sizes={4}
    ds1 = u32[4] dynamic-slice(ar1, offset), dynamic_slice_sizes={4}
    ROOT add = u32[4] add(ds0, ds1)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));

  const ErrorSpec es{1e-5, 1e-5};
  LiteralTestUtil::ExpectR1Equal<uint32_t>({26, 30, 34, 38}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({42, 46, 50, 54}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AllReduceReassociate)) {
  const char* const kModuleStr = R"(
  HloModule m
  sum {
    a = f32[] parameter(0)
    b = f32[] parameter(1)
    ROOT add.2 = f32[] add(a, b)
  }

  ENTRY main {
    c0 = f32[8] constant({  1,  2,  3,  4,  5,  6,  7,  8})
    c1 = f32[8] constant({ 11, 12, 13, 14, 15, 16, 17, 18})
    c2 = f32[8] constant({  2,  3,  4,  5,  6,  7,  8,  9})
    c3 = f32[8] constant({ 12, 13, 14, 15, 16, 17, 18, 19})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data0 = c0 for replica 0 and c1 for replica 1
    data0 = f32[8] select(pb, c0, c1)
    // data1 = c2 for replica 0 and c3 for replica 1
    data1 = f32[8] select(pb, c2, c3)

    ar0 = f32[8] all-reduce(data0), replica_groups={}, to_apply=sum
    ar1 = f32[8] all-reduce(data1), replica_groups={}, to_apply=sum
    ROOT add = f32[8] add(ar0, ar1)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));

  const ErrorSpec es{1e-5, 1e-5};
  EXPECT_TRUE(LiteralTestUtil::NearOrEqual(results[0], results[1], es));
  LiteralTestUtil::ExpectR1Near<float>(
      {26.0, 30.0, 34.0, 38.0, 42.0, 46.0, 50.0, 54.0}, results[0], es);
}

XLA_TEST_F(CollectiveOpsTest,
           DISABLED_ON_CPU(AllGatherBroadcastReorder_NonUniform)) {
  const char* const kModuleStr = R"(
  HloModule m

  ENTRY main {
    c0 = u32[2, 3] constant({{ 1,  2,  3}, { 4, 5, 6}})
    c1 = u32[2, 3] constant({{10, 11, 12}, {13, 14, 15}})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[2, 3] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[2, 3] select(pb, c0, c1)
    bc = u32[2, 4, 3] broadcast(data), dimensions={0, 2}
    ROOT ag = u32[2, 4, 6] all-gather(bc), dimensions={2}, replica_groups={{0, 1}}
  }
  )";

  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));

  EXPECT_TRUE(LiteralTestUtil::Equal(results[0], results[1]));
  LiteralTestUtil::ExpectR3Equal<uint32_t>({{{1, 2, 3, 10, 11, 12},
                                             {1, 2, 3, 10, 11, 12},
                                             {1, 2, 3, 10, 11, 12},
                                             {1, 2, 3, 10, 11, 12}},
                                            {{4, 5, 6, 13, 14, 15},
                                             {4, 5, 6, 13, 14, 15},
                                             {4, 5, 6, 13, 14, 15},
                                             {4, 5, 6, 13, 14, 15}}},
                                           results[0]);
}

XLA_TEST_F(CollectiveOpsTest,
           DISABLED_ON_CPU(AllGatherBroadcastReorder_Uniform)) {
  const char* const kModuleStr = R"(
  HloModule m

  ENTRY main {
    c0 = u32[2, 3] constant({{ 1,  2,  3}, { 4, 5, 6}})
    c1 = u32[2, 3] constant({{10, 11, 12}, {13, 14, 15}})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[2, 3] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[2, 3] select(pb, c0, c1)
    bc = u32[2, 4, 3] broadcast(data), dimensions={0, 2}
    ROOT ag = u32[2, 8, 3] all-gather(bc), dimensions={1}, replica_groups={{0, 1}}
  }
  )";

  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  EXPECT_TRUE(LiteralTestUtil::Equal(results[0], results[1]));
  LiteralTestUtil::ExpectR3Equal<uint32_t>({{{1, 2, 3},
                                             {1, 2, 3},
                                             {1, 2, 3},
                                             {1, 2, 3},
                                             {10, 11, 12},
                                             {10, 11, 12},
                                             {10, 11, 12},
                                             {10, 11, 12}},
                                            {{4, 5, 6},
                                             {4, 5, 6},
                                             {4, 5, 6},
                                             {4, 5, 6},
                                             {13, 14, 15},
                                             {13, 14, 15},
                                             {13, 14, 15},
                                             {13, 14, 15}}},
                                           results[0]);
}

XLA_TEST_F(CollectiveOpsTest, AllGather_16BitInt) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id32 = u32[] replica-id()
    id = u16[] convert(id32)
    id2 = u16[1, 2] broadcast(id), dimensions={}
    a0 = u16[1, 2] constant({{10, 15}})
    a1 = u16[1, 2] add(id2, a0)
    allgather = u16[2, 2] all-gather(a1), dimensions={0}
    ROOT out = u16[4] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint16_t>({10, 15, 11, 16}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, AllToAll_16BitInt) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id32 = u32[] replica-id()
    id = u16[] convert(id32)
    id2 = u16[2] broadcast(id), dimensions={}
    a0 = u16[2] constant({10, 15})
    a1 = u16[2] add(id2, a0)
    ROOT a2a = u16[2] all-to-all(a1), dimensions={0}
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({10, 11}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({15, 16}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, CollectivePermute_16BitInt) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id32 = u32[] replica-id()
    id = u16[] convert(id32)
    id2 = u16[2] broadcast(id), dimensions={}
    a0 = u16[2] constant({10, 15})
    a1 = u16[2] add(id2, a0)
    ROOT cp = u16[2] collective-permute(a1), source_target_pairs={{0,1}, {1,0}}
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({11, 16}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({10, 15}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, AllReduce_16BitInt) {
  const char* const kModuleStr = R"(
  HloModule test

  sum {
    a = u16[] parameter(0)
    b = u16[] parameter(1)
    ROOT add.2 = u16[] add(a, b)
  }

  ENTRY test_computation {
    id32 = u32[] replica-id()
    id = u16[] convert(id32)
    id2 = u16[2] broadcast(id), dimensions={}
    a0 = u16[2] constant({10, 15})
    a1 = u16[2] add(id2, a0)
    ROOT cp = u16[2] all-reduce(a1), replica_groups={}, to_apply=sum
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint16_t>({21, 31}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, ReduceScatter_16BitInt) {
  const char* const kModuleStr = R"(
  HloModule test

  sum {
    a = u16[] parameter(0)
    b = u16[] parameter(1)
    ROOT add.2 = u16[] add(a, b)
  }

  ENTRY test_computation {
    id32 = u32[] replica-id()
    id = u16[] convert(id32)
    id2 = u16[2] broadcast(id), dimensions={}
    a0 = u16[2] constant({10, 15})
    a1 = u16[2] add(id2, a0)
    ROOT cp = u16[1]reduce-scatter(a1), dimensions={0}, replica_groups={}, to_apply=sum
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({21}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint16_t>({31}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, AllReduceBFloat16Min) {
  const char* const kModuleStr = R"(
  HloModule test

  min {
    a = bf16[] parameter(0)
    b = bf16[] parameter(1)
    ROOT min.2 = bf16[] minimum(a, b)
  }

  ENTRY test_computation {
    id32 = u32[] replica-id()
    one = u32[] constant(1)
    id32_1 = u32[] add(id32, one)
    id = bf16[] convert(id32_1)
    id2 = bf16[2] broadcast(id), dimensions={}
    ROOT cp = bf16[2] all-reduce(id2), replica_groups={}, to_apply=min
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  const bfloat16 one = static_cast<bfloat16>(1.0f);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<bfloat16>({one, one}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncAllGather)) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[1, 2] broadcast(id), dimensions={}
    a0 = u32[1, 2] constant({{10, 15}})
    a1 = u32[1, 2] add(id2, a0)
    ags = (u32[1, 2], u32[2, 2]) all-gather-start(a1), dimensions={0}, backend_config="{\"is_sync\":false}"
    allgather = u32[2,2] all-gather-done(ags)
    ROOT out = u32[4] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 15, 11, 16}, result);
  }
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncReduceScatter)) {
  const char* const kModuleStr = R"(
  HloModule test
  add {
    lhs = u32[] parameter(0)
    rhs = u32[] parameter(1)
    ROOT add = u32[] add(lhs, rhs)
  }

  // XLA HLO does not have reduce-scatter-start/reduce-scatter-done op, but
  // uses the generic async-start/async-done ops.
  reduce_scatter {
    p0 = u32[8] parameter(0)
    ROOT result = u32[4] reduce-scatter(p0), replica_groups={},
                      dimensions={0}, to_apply=add
  }

  ENTRY main {
    c0 = u32[8] constant({1, 2, 3, 4, 5, 6, 7, 8})
    c1 = u32[8] constant({10, 11, 12, 13, 14, 15, 16, 17})
    zero = u32[] constant(0)
    id = u32[] replica-id()
    p = pred[] compare(id, zero), direction=EQ
    pb = pred[8] broadcast(p), dimensions={}
    // data = c0 for replica 0 and c1 for replica 1
    data = u32[8] select(pb, c0, c1)
    rs-start = ((u32[8]{0}), u32[4]{0}) async-start(u32[8]{0} %data), calls=reduce_scatter, backend_config="{\"is_sync\":false}"
    ROOT %ars = u32[4]{0} async-done(((u32[8]{0}), u32[4]{0}) %rs-start), calls=reduce_scatter
  }
  )";

  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));
  LiteralTestUtil::ExpectR1Equal<uint32_t>({11, 13, 15, 17}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({19, 21, 23, 25}, results[1]);
}

XLA_TEST_F(CollectiveOpsTest, DISABLED_ON_CPU(AsyncAllToAll)) {
  const char* const kModuleStr = R"(
  HloModule test

  all_to_all {
    p0 = u32[2] parameter(0)
    ROOT result = u32[2] all-to-all(p0), dimensions={0}
  }

  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[2] broadcast(id), dimensions={}
    a0 = u32[2] constant({10, 15})
    a1 = u32[2] add(id2, a0)
    a2a-start = ((u32[2]), u32[2]) async-start(u32[2] %a1), calls=all_to_all, backend_config="{\"is_sync\":false}"
    ROOT a2s = u32[2] async-done(a2a-start), calls=all_to_all
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/false));
  ASSERT_EQ(results.size(), kNumReplicas);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({10, 11}, results[0]);
  LiteralTestUtil::ExpectR1Equal<uint32_t>({15, 16}, results[1]);
}

// Test for all-gather with unit dims to verify that dimension check works
// correctly in the presence of unit dimensions.
XLA_TEST_F(CollectiveOpsTest, AllGather_Dim1UnitDimensions) {
  const char* const kModuleStr = R"(
  HloModule test
  ENTRY test_computation {
    id = u32[] replica-id()
    id2 = u32[1, 1, 2, 1, 2] broadcast(id), dimensions={}
    offset = u32[4] iota(), iota_dimension=0
    offset_reshape = u32[1, 1, 2, 1, 2] reshape(offset)
    agi = u32[1, 1, 2, 1, 2] add(id2, offset_reshape)
    allgather = u32[1, 1, 4, 1, 2] all-gather(agi), dimensions={2}
    ROOT out = u32[8] reshape(allgather)
  }
  )";
  const int64_t kNumReplicas = 2;
  HloModuleConfig config =
      GetModuleConfigForTest(/*replica_count=*/kNumReplicas);
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(kModuleStr, config));

  TF_ASSERT_OK_AND_ASSIGN(
      std::vector<Literal> results,
      ExecuteReplicated(std::move(module), {}, kNumReplicas,
                        /*use_threads=*/true, /*run_hlo_passes=*/true));
  ASSERT_EQ(results.size(), kNumReplicas);
  for (const Literal& result : results) {
    LiteralTestUtil::ExpectR1Equal<uint32_t>({0, 1, 2, 3, 1, 2, 3, 4}, result);
  }
}

}  // namespace
}  // namespace xla
