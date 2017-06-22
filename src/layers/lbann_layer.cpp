////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
// lbann_layer .hpp .cpp - Parent class for all layer types
////////////////////////////////////////////////////////////////////////////////

#include "lbann/layers/lbann_layer.hpp"
#include "lbann/utils/lbann_timer.hpp"
#include "lbann/models/lbann_model.hpp"
#include "lbann/io/lbann_file_io.hpp"
#include "lbann/io/lbann_persist.hpp"
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;
using namespace El;

namespace lbann {
/// Matrices should be in MC,MR distributions
template <>
void Layer::initialize_distributed_matrices<data_layout::MODEL_PARALLEL>() {
  m_prev_activations    = new DistMat(m_comm->get_model_grid());
  m_activations         = new DistMat(m_comm->get_model_grid());
  m_prev_error_signal   = new DistMat(m_comm->get_model_grid());
  m_error_signal        = new DistMat(m_comm->get_model_grid());

  /// Instantiate these view objects but do not allocate data for them
  m_prev_activations_v  = new DistMat(m_comm->get_model_grid());
  m_activations_v       = new DistMat(m_comm->get_model_grid());
  m_prev_error_signal_v = new DistMat(m_comm->get_model_grid());
  m_error_signal_v      = new DistMat(m_comm->get_model_grid());
}

/// Weight matrices should be in Star,Star and data matrices Star,VC distributions
template<>
void Layer::initialize_distributed_matrices<data_layout::DATA_PARALLEL>() {
  m_prev_activations    = new StarVCMat(m_comm->get_model_grid());
  m_activations         = new StarVCMat(m_comm->get_model_grid());
  m_prev_error_signal   = new StarVCMat(m_comm->get_model_grid());
  m_error_signal        = new StarVCMat(m_comm->get_model_grid());

  /// Instantiate these view objects but do not allocate data for them
  m_prev_activations_v  = new StarVCMat(m_comm->get_model_grid());
  m_activations_v       = new StarVCMat(m_comm->get_model_grid());
  m_prev_error_signal_v = new StarVCMat(m_comm->get_model_grid());
  m_error_signal_v      = new StarVCMat(m_comm->get_model_grid());
}
}

lbann::Layer::Layer(const uint index,
                    lbann_comm *comm,
                    uint mbsize)
  : m_index(index),
    m_comm(comm),
    m_type(layer_type::INVALID), m_prev_layer_type(layer_type::INVALID), m_next_layer_type(layer_type::INVALID),
    m_execution_mode(execution_mode::training),
    m_cudnn(nullptr),
    m_mini_batch_size(mbsize),
    m_effective_mbsize(mbsize)
{
  set_name("the name has not been set for this layer");

  fp_input = NULL;
  bp_input = NULL;
  m_neural_network_model = NULL;

  m_using_gpus = false;
  m_prev_layer_using_gpus = false;
  m_next_layer_using_gpus = false;
#ifdef __LIB_CUDNN
  fp_input_d = NULL;
  bp_input_d = NULL;
  m_fp_input_pinned = false;
  m_fp_output_pinned = false;
  m_bp_input_pinned = false;
  m_bp_output_pinned = false;
#endif

  reset_counters();

}

lbann::Layer::~Layer() {
#ifdef __LIB_CUDNN
  if(m_fp_input_pinned) {
    m_cudnn->unpin_matrix(*m_prev_activations);
  }
  if(m_fp_output_pinned) {
    m_cudnn->unpin_matrix(*m_activations);
  }
  if(m_bp_input_pinned) {
    m_cudnn->unpin_matrix(*m_prev_error_signal);
  }
  if(m_bp_output_pinned) {
    m_cudnn->unpin_matrix(*m_error_signal);
  }
#endif
  delete m_prev_error_signal;
  delete m_error_signal;
  delete m_activations;
  delete m_prev_activations;
  delete m_prev_error_signal_v;
  delete m_error_signal_v;
  delete m_activations_v;
  delete m_prev_activations_v;
}

void lbann::Layer::forwardProp() {
  double fp_start = get_time();

#ifdef __LIB_CUDNN
  // Pin host memory if needed for GPU memory transfers
  if(m_using_gpus && !m_prev_layer_using_gpus && !m_fp_input_pinned) {
    if(fp_input != NULL
       && m_prev_activations->DistData().colDist == fp_input->DistData().colDist
       && m_prev_activations->DistData().rowDist == fp_input->DistData().rowDist) {
      m_cudnn->pin_matrix(*fp_input);
    }
    else {
      m_cudnn->pin_matrix(*m_prev_activations);
    }
    m_fp_input_pinned = true;
  }
  if(m_using_gpus && !m_next_layer_using_gpus && !m_fp_output_pinned) {
    m_cudnn->pin_matrix(*m_activations);
    m_fp_output_pinned = true;
  }
#endif

  // Get incoming activations and convert matrix distribution if necessary
  if(fp_input != NULL) { // Input layers will not have a valid fp_input
    if(m_prev_activations->DistData().colDist == fp_input->DistData().colDist
       && m_prev_activations->DistData().rowDist == fp_input->DistData().rowDist) {
      View(*m_prev_activations, *fp_input);
    } else {
      Copy(*fp_input, *m_prev_activations);
    }
  }

  // Set matrix views based on current mini-batch size
  fp_set_std_matrix_view();

#ifdef __LIB_CUDNN
  // Transfer inputs from CPU to GPUs if needed
  if(m_using_gpus) {
    if(!m_prev_layer_using_gpus) {
      m_cudnn->scatter_to_gpus(m_prev_activations_d,
                               m_prev_activations_v->LockedMatrix(),
                               m_mini_batch_size_per_gpu);
    } else {
      m_prev_activations_d = *fp_input_d;
    }
  }
#endif

  // Apply layer's compute function
  double fp_compute_start = get_time();
  //cerr << "CALLING fp_compute()\n";
  fp_compute();
  //cerr << "DONE CALLING fp_compute()\n";
  fp_compute_time += get_time() - fp_compute_start;

#ifdef __LIB_CUDNN
  // Transfer outputs from GPUs to CPU if needed
  if(m_using_gpus && !m_next_layer_using_gpus) {
    if(!m_fp_output_pinned) {
      m_cudnn->pin_matrix(*m_activations);
    }
    m_cudnn->gather_from_gpus(m_activations_v->Matrix(),
                              m_activations_d,
                              m_mini_batch_size_per_gpu);
    m_cudnn->synchronize();
  }
#endif

  fp_time += get_time() - fp_start;
}

void lbann::Layer::backProp() {
  double bp_start = get_time();

#ifdef __LIB_CUDNN
  // Pin host memory if needed for GPU memory transfers
  if(m_using_gpus && !m_next_layer_using_gpus && !m_bp_input_pinned) {
    if(bp_input != NULL
       && m_prev_error_signal->DistData().colDist == bp_input->DistData().colDist
       && m_prev_error_signal->DistData().rowDist == bp_input->DistData().rowDist) {
      m_cudnn->pin_matrix(*bp_input);
    }
    else {
      m_cudnn->pin_matrix(*m_prev_error_signal);
    }
    m_bp_input_pinned = true;
  }
  if(m_using_gpus && !m_prev_layer_using_gpus && !m_bp_output_pinned) {
    m_cudnn->pin_matrix(*m_error_signal);
    m_bp_output_pinned = true;
  }
#endif

  // Get incoming loss and convert matrix distribution if necessary
  if(bp_input != NULL) { // Target layers will not have a valid bp_input
    if(m_prev_error_signal->DistData().colDist == bp_input->DistData().colDist
       && m_prev_error_signal->DistData().rowDist == bp_input->DistData().rowDist) {
      View(*m_prev_error_signal, *bp_input);
    } else {
      Copy(*bp_input, *m_prev_error_signal);
    }
  }

  // Set the view for all of the standard matrices based on the
  // current mini-batch size
  bp_set_std_matrix_view();

#ifdef __LIB_CUDNN
  // Transfer inputs from CPU to GPUs
  if(m_using_gpus) {
    if(!m_next_layer_using_gpus) {
      m_cudnn->scatter_to_gpus(m_prev_error_signal_d,
                               m_prev_error_signal_v->LockedMatrix(),
                               m_mini_batch_size_per_gpu);
    } else {
      m_prev_error_signal_d = *bp_input_d;
    }
  }
#endif

  // Backprop the compute function.
  double bp_compute_start = get_time();
  bp_compute();
  bp_compute_time += get_time() - bp_compute_start;

#ifdef __LIB_CUDNN
  // Transfer outputs from GPUs to CPU
  if(m_using_gpus && !m_prev_layer_using_gpus) {
    m_cudnn->gather_from_gpus(m_error_signal_v->Matrix(),
                              m_error_signal_d,
                              m_mini_batch_size_per_gpu);
    m_cudnn->synchronize();
  }
#endif

  bp_time += get_time() - bp_start;
}

bool lbann::Layer::update() {
  bool layer_done = false;
  // Apply any updates.
  double update_compute_start = get_time();
  layer_done = update_compute();
  update_time += get_time() - update_compute_start;
  return layer_done;
}

void lbann::Layer::summarize(lbann_summary& summarizer, int64_t step) {
  // TODO: implement summarizer functions for other matrix distributions
  std::string prefix = "layer" + std::to_string(static_cast<long long>(m_index)) + "/";
  summarizer.reduce_scalar(prefix + "fp_time", fp_time, step);
  summarizer.reduce_scalar(prefix + "bp_time", bp_time, step);
  summarizer.reduce_scalar(prefix + "update_time", update_time, step);
  prefix = "layer" + std::to_string(static_cast<long long>(m_index)) +
    "/activations/";
  summarizer.reduce_mean(prefix + "mean", *m_activations, step);
  summarizer.reduce_min(prefix + "min", *m_activations, step);
  summarizer.reduce_max(prefix + "max", *m_activations, step);
  summarizer.reduce_stdev(prefix + "stdev", *m_activations, step);
  prefix = "layer" + std::to_string(static_cast<long long>(m_index)) +
    "/error_signal/";
  summarizer.reduce_mean(prefix + "mean", *m_error_signal, step);
  summarizer.reduce_min(prefix + "min", *m_error_signal, step);
  summarizer.reduce_max(prefix + "max", *m_error_signal, step);
  summarizer.reduce_stdev(prefix + "stdev", *m_error_signal, step);
  reset_counters();
}

void lbann::Layer::setup(int num_prev_neurons) {
  m_num_prev_neurons = num_prev_neurons;
}

void lbann::Layer::check_setup() {}

ElMat *lbann::Layer::fp_output() {
  return m_activations;
}

ElMat *lbann::Layer::bp_output() {
  return m_error_signal;
}

void lbann::Layer::setup_fp_input(ElMat *input) {
  this->fp_input = input;
}

void lbann::Layer::setup_bp_input(ElMat *input) {
  this->bp_input = input;
}

#ifdef __LIB_CUDNN
std::vector<DataType *> *lbann::Layer::fp_output_d() {
  if(m_using_gpus) {
    return &m_activations_d;
  } else {
    return NULL;
  }
}

std::vector<DataType *> *lbann::Layer::bp_output_d() {
  if(m_using_gpus) {
    return &m_error_signal_d;
  } else {
    return NULL;
  }
}

void lbann::Layer::setup_fp_input_d(std::vector<DataType *> *fp_input_d) {
  this->fp_input_d = fp_input_d;
}

void lbann::Layer::setup_bp_input_d(std::vector<DataType *> *bp_input_d) {
  this->bp_input_d = bp_input_d;
}
#endif

void lbann::Layer::set_prev_layer_type(layer_type type) {
  this->m_prev_layer_type = type;
}

void lbann::Layer::set_next_layer_type(layer_type type) {
  this->m_next_layer_type = type;
}

bool lbann::Layer::using_gpus() const {
  return m_using_gpus;
}

void lbann::Layer::set_prev_layer_using_gpus(bool using_gpus) {
  m_prev_layer_using_gpus = using_gpus;
}

void lbann::Layer::set_next_layer_using_gpus(bool using_gpus) {
  m_next_layer_using_gpus = using_gpus;
}

bool lbann::Layer::saveToCheckpoint(int fd, const char *filename, uint64_t *bytes) {
  //writeDist(fd, filename, *m_weights, bytes);

  // Need to catch return value from function
  // m_optimizer->saveToCheckpoint(fd, filename, bytes);
  return true;
}

bool lbann::Layer::loadFromCheckpoint(int fd, const char *filename, uint64_t *bytes) {
  // TODO: implement reader for other matrix distributions
  //readDist(fd, filename, (DistMat&) *m_weights, bytes);

  // Need to catch return value from function
  // m_optimizer->loadFromCheckpoint(fd, filename, bytes);
  return true;
}

bool lbann::Layer::saveToCheckpointShared(lbann::persist& p) {
  return true;
}

bool lbann::Layer::loadFromCheckpointShared(lbann::persist& p) {
  return true;
}

void lbann::Layer::fp_set_std_matrix_view() {
  Int cur_mini_batch_size = m_neural_network_model->get_current_mini_batch_size();
  View(*m_prev_activations_v, *m_prev_activations, ALL, IR(0, cur_mini_batch_size));
  View(*m_activations_v, *m_activations, ALL, IR(0, cur_mini_batch_size));

  // Update the layer's effective mini-batch size so it averages properly.
  /// @todo BVE FIXME This will cause a bug when you are on the last
  /// iteration and the size of the current mini-batch equals the normal
  /// mini-batch size.  In this case one of the ranks gets out of sync
  /// To fix this, we need a flag for when we are on the last mini-batch
  if(cur_mini_batch_size != m_mini_batch_size || 1) {
    // When the current mini-batch is partial, check with the other
    // models to figure out the entire size of the complete mini-batch
    Int total_mini_batch_size = m_comm->intermodel_allreduce((Int) cur_mini_batch_size);
    set_effective_minibatch_size(total_mini_batch_size);
  } else {
    set_effective_minibatch_size(cur_mini_batch_size * m_comm->get_num_models());
  }
}

void lbann::Layer::bp_set_std_matrix_view() {
  int64_t cur_mini_batch_size = m_neural_network_model->get_current_mini_batch_size();
  View(*m_prev_activations_v, *m_prev_activations, ALL, IR(0, cur_mini_batch_size));
  View(*m_activations_v, *m_activations, ALL, IR(0, cur_mini_batch_size));
  if(m_prev_error_signal->Height() > 0) {
    View(*m_prev_error_signal_v, *m_prev_error_signal, ALL,
         IR(0, cur_mini_batch_size));
  }
  View(*m_error_signal_v, *m_error_signal, ALL, IR(0, cur_mini_batch_size));
}
