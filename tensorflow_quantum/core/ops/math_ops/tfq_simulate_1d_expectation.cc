/* Copyright 2020 The TensorFlow Quantum Authors. All Rights Reserved.

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
#include <vector>

#include "../qsim/lib/circuit.h"
#include "../qsim/lib/formux.h"
#include "../qsim/lib/gate_appl.h"
#include "../qsim/lib/gates_cirq.h"
#include "../qsim/lib/mps_simulator.h"
#include "../qsim/lib/mps_statespace.h"
#include "../qsim/lib/seqfor.h"
#include "../qsim/lib/simmux.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/lib/core/error_codes.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow_quantum/core/ops/parse_context.h"
#include "tensorflow_quantum/core/proto/pauli_sum.pb.h"
#include "tensorflow_quantum/core/proto/program.pb.h"
#include "tensorflow_quantum/core/src/program_resolution.h"
#include "tensorflow_quantum/core/src/util_qsim.h"

namespace tfq {

using ::tensorflow::Status;
using ::tfq::proto::PauliSum;
using ::tfq::proto::Program;

typedef qsim::Cirq::GateCirq<float> QsimGate;
typedef qsim::Circuit<QsimGate> QsimCircuit;

class TfqSimulateMPS1DExpectationOp : public tensorflow::OpKernel {
 public:
  explicit TfqSimulateMPS1DExpectationOp(
      tensorflow::OpKernelConstruction* context)
      : OpKernel(context) {
    // Get the bond dimension of MPS
    // Checked that bond_dim is a positive integer >= 2 by QSim definition.
    OP_REQUIRES_OK(context, context->GetAttr("bond_dim", &bond_dim_));
  }

  void Compute(tensorflow::OpKernelContext* context) override {
    // TODO (mbbrough): add more dimension checks for other inputs here.
    const int num_inputs = context->num_inputs();
    OP_REQUIRES(context, num_inputs == 4,
                tensorflow::errors::InvalidArgument(absl::StrCat(
                    "Expected 4 inputs, got ", num_inputs, " inputs.")));

    // Create the output Tensor.
    const int output_dim_batch_size = context->input(0).dim_size(0);
    const int output_dim_op_size = context->input(3).dim_size(1);
    tensorflow::TensorShape output_shape;
    std::cout << "batch_size = " << output_dim_batch_size << std::endl;
    std::cout << "pauli sums size = " << output_dim_op_size << std::endl;
    output_shape.AddDim(output_dim_batch_size);
    output_shape.AddDim(output_dim_op_size);

    tensorflow::Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));
    auto output_tensor = output->matrix<float>();

    // Parse program protos.
    std::vector<Program> programs;
    std::vector<int> num_qubits;
    std::vector<std::vector<PauliSum>> pauli_sums;
    OP_REQUIRES_OK(context, GetProgramsAndNumQubits(context, &programs,
                                                    &num_qubits, &pauli_sums));
    std::cout << "parse program protos" << std::endl;

    std::vector<SymbolMap> maps;
    OP_REQUIRES_OK(context, GetSymbolMaps(context, &maps));

    OP_REQUIRES(context, programs.size() == maps.size(),
                tensorflow::errors::InvalidArgument(absl::StrCat(
                    "Number of circuits and symbol_values do not match. Got ",
                    programs.size(), " circuits and ", maps.size(),
                    " symbol values.")));

    OP_REQUIRES_OK(context, CheckQubitsIn1D(&programs));
    std::cout << "CheckQubitsIn1D" << std::endl;

    // Construct qsim circuits.
    std::vector<QsimCircuit> qsim_circuits(programs.size(), QsimCircuit());
    std::vector<QsimFusedCircuit> fused_circuits(programs.size(),
                                                 QsimFusedCircuit({}));
    Status parse_status = Status::OK();
    auto p_lock = tensorflow::mutex();
    auto construct_f = [&](int start, int end) {
      for (int i = start; i < end; i++) {
        Status local =
            QsimCircuitFromProgram(programs[i], maps[i], num_qubits[i],
                                   &qsim_circuits[i], &fused_circuits[i]);
        NESTED_FN_STATUS_SYNC(parse_status, local, p_lock);
      }
    };

    const int num_cycles = 1000;
    context->device()->tensorflow_cpu_worker_threads()->workers->ParallelFor(
        output_dim_batch_size, num_cycles, construct_f);
    OP_REQUIRES_OK(context, parse_status);
    std::cout << "QsimCircuitFromProgram" << std::endl;

    int max_num_qubits = 0;
    for (const int num : num_qubits) {
      max_num_qubits = std::max(max_num_qubits, num);
    }

    // Cross reference with standard google cloud compute instances
    // Memory ~= 2 * num_threads * (2 * 64 * 2 ** num_qubits in circuits)
    // e2s2 = 2 CPU, 8GB -> Can safely do 25 since Memory = 4GB
    // e2s4 = 4 CPU, 16GB -> Can safely do 25 since Memory = 8GB
    // ...
    if (true || max_num_qubits >= 26 || programs.size() == 1) {
      std::cout << "Enter ComputeLarge" << std::endl;
      ComputeLarge(num_qubits, qsim_circuits, pauli_sums, context,
                   &output_tensor);
    } else {
      std::cout << "THIS SHOULD NOT BE PRINTED OUT" << std::endl;
      ComputeSmall(num_qubits, max_num_qubits, qsim_circuits, pauli_sums,
                   context, &output_tensor);
    }
  }

 private:
  int bond_dim_;

  void ComputeLarge(
      const std::vector<int>& num_qubits,
      const std::vector<QsimCircuit>& qsim_circuits,
      const std::vector<std::vector<PauliSum>>& pauli_sums,
      tensorflow::OpKernelContext* context,
      tensorflow::TTypes<float, 2>::Matrix* output_tensor) {
    // Instantiate qsim objects.
    using Simulator = qsim::mps::MPSSimulator<qsim::For, float>;
    using StateSpace = Simulator::MPSStateSpace_;

    // // Begin simulation.
    int largest_nq = 1;
    // Note: ForArgs in MPSSimulator and MPSStateState are currently unused.
    // So, this 1 is a dummy for qsim::For.
    Simulator sim = Simulator(1);
    StateSpace ss = StateSpace(1);
    auto sv = ss.Create(largest_nq, bond_dim_);
    auto scratch = ss.Create(largest_nq, bond_dim_);

    // Simulate programs one by one. Parallelizing over state vectors
    // we no longer parallelize over circuits. Each time we encounter a
    // a larger circuit we will grow the Statevector as necessary.
    for (int i = 0; i < qsim_circuits.size(); i++) {
      std::cout << "ComputeLarge > runs circuit" << i << std::endl;
      int nq = num_qubits[i];

      if (nq > largest_nq) {
        // need to switch to larger statespace.
        largest_nq = nq;
        sv = ss.Create(largest_nq, bond_dim_);
        scratch = ss.Create(largest_nq, bond_dim_);
      }
      // TODO: add heuristic here so that we do not always recompute
      //  the state if there is a possibility that circuit[i] and
      //  circuit[i + 1] produce the same state.
      ss.SetStateZero(sv);
      auto qsim_gates = qsim_circuits[i].gates;
      std::cout << "ComputeLarge > QsimGate size = " << qsim_gates.size() << std::endl;
      for (int j = 0; j < qsim_gates.size(); j++) {
        std::cout << "ComputeLarge > ApplyGate " << j << " qsize = " << qsim_gates[j].qubits.size() << std:: endl;
        std::cout << "ComputeLarge > ApplyGate > Qubits = ";
        for (int k = 0; k < qsim_gates[j].qubits.size(); k++) {
          std::cout << qsim_gates[j].qubits[k] << ", ";
        }
        std::cout << std::endl;
        std::cout << "ComputeLarge > ApplyGate > Gate = ";
        for (int k = 0; k < qsim_gates[j].matrix.size(); k++) {
          std::cout << qsim_gates[j].matrix[k] << ", ";
        }
        std::cout << std::endl;
        sim.ApplyGate(qsim_gates[j].qubits, qsim_gates[j].matrix.data(), sv);
      }
      for (int j = 0; j < pauli_sums[i].size(); j++) {
        // (#679) Just ignore empty program
        if (qsim_gates.size() == 0) {
          std::cout << "ComputeLarge > result(" << i << ", " << j << ") = -2.0" << std::endl;
          (*output_tensor)(i, j) = -2.0;
          continue;
        }
        float exp_v = 0.0;
        // OP_REQUIRES_OK(context, ComputeExpectationQsim(pauli_sums[i][j], sim, ss,
        //                                                sv, scratch, &exp_v));
        std::cout << "ComputeLarge > result(" << i << ", " << j << ") = " << exp_v << std::endl;
        //(*output_tensor)(i, j) = exp_v;
      }
    }
  }

  void ComputeSmall(
      const std::vector<int>& num_qubits, const int max_num_qubits,
      const std::vector<QsimCircuit>& qsim_circuits,
      const std::vector<std::vector<PauliSum>>& pauli_sums,
      tensorflow::OpKernelContext* context,
      tensorflow::TTypes<float>::Matrix* output_tensor) {
    // using Simulator = qsim::mps::MPSSimulator<qsim::For, float>;
    // using StateSpace = Simulator::MPSStateSpace_;

    // const int output_dim_op_size = output_tensor->dimension(1);

    // Status compute_status = Status::OK();
    // auto c_lock = tensorflow::mutex();
    // auto DoWork = [&](int start, int end) {
    //   int old_batch_index = -2;
    //   int cur_batch_index = -1;
    //   int largest_nq = 1;
    //   int cur_op_index;

    //   // Note: ForArgs in MPSSimulator and MPSStateState are currently unused.
    //   // So, this 1 is a dummy for qsim::For.
    //   Simulator sim = Simulator(1);
    //   StateSpace ss = StateSpace(1);
    //   auto sv = ss.Create(largest_nq, bond_dim_);
    //   auto scratch = ss.Create(largest_nq, bond_dim_);
    //   for (int i = start; i < end; i++) {
    //     cur_batch_index = i / output_dim_op_size;
    //     cur_op_index = i % output_dim_op_size;

    //     const int nq = num_qubits[cur_batch_index];

    //     // (#679) Just ignore empty program
    //     auto qsim_gates = qsim_circuits[cur_batch_index].gates;
    //     if (qsim_gates.size() == 0) {
    //       (*output_tensor)(cur_batch_index, cur_op_index) = -2.0;
    //       continue;
    //     }

    //     if (cur_batch_index != old_batch_index) {
    //       // We've run into a new state vector we must compute.
    //       // Only compute a new state vector when we have to.
    //       if (nq > largest_nq) {
    //         largest_nq = nq;
    //         sv = ss.Create(largest_nq, bond_dim_);
    //         scratch = ss.Create(largest_nq, bond_dim_);
    //       }
    //       // no need to update scratch_state since ComputeExpectation
    //       // will take care of things for us.
    //       ss.SetStateZero(sv);
    //       for (int j = 0; j < qsim_gates.size(); j++) {
    //         //ApplyFusedGateMPS(sim, qsim_circuits[cur_batch_index][j], sv);
    //       }
    //     }

    //     float exp_v = 0.0;
    //     // NESTED_FN_STATUS_SYNC(
    //     //     compute_status,
    //     //     ComputeExpectationMPS(pauli_sums[cur_batch_index][cur_op_index],
    //     //                           sim, ss, sv, scratch, &exp_v),
    //     //     c_lock);
    //     (*output_tensor)(cur_batch_index, cur_op_index) = exp_v;
    //     old_batch_index = cur_batch_index;
    //   }
    // };

    // const int64_t num_cycles =
    //     200 * (int64_t(1) << static_cast<int64_t>(max_num_qubits));
    // context->device()->tensorflow_cpu_worker_threads()->workers->ParallelFor(
    //     qsim_circuits.size() * output_dim_op_size, num_cycles, DoWork);
    // OP_REQUIRES_OK(context, compute_status);
  }
};

REGISTER_KERNEL_BUILDER(
    Name("TfqSimulateMPS1DExpectation").Device(tensorflow::DEVICE_CPU),
    TfqSimulateMPS1DExpectationOp);

REGISTER_OP("TfqSimulateMPS1DExpectation")
    .Input("programs: string")
    .Input("symbol_names: string")
    .Input("symbol_values: float")
    .Input("pauli_sums: string")
    .Output("expectations: float")
    .Attr("bond_dim: int >= 2 = 2")
    .SetShapeFn([](tensorflow::shape_inference::InferenceContext* c) {
      tensorflow::shape_inference::ShapeHandle programs_shape;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(0), 1, &programs_shape));

      tensorflow::shape_inference::ShapeHandle symbol_names_shape;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(1), 1, &symbol_names_shape));

      tensorflow::shape_inference::ShapeHandle symbol_values_shape;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(2), 2, &symbol_values_shape));

      tensorflow::shape_inference::ShapeHandle pauli_sums_shape;
      TF_RETURN_IF_ERROR(c->WithRank(c->input(3), 2, &pauli_sums_shape));

      tensorflow::shape_inference::DimensionHandle output_rows =
          c->Dim(programs_shape, 0);
      tensorflow::shape_inference::DimensionHandle output_cols =
          c->Dim(pauli_sums_shape, 1);
      c->set_output(0, c->Matrix(output_rows, output_cols));

      return tensorflow::Status::OK();
    });

}  // namespace tfq
