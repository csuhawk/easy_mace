// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <memory>

#include "mace/core/net.h"
#include "mace/core/types.h"
#include "mace/public/mace.h"

#ifdef MACE_ENABLE_OPENCL
#include "mace/core/runtime/opencl/opencl_runtime.h"
#endif  // MACE_ENABLE_OPENCL

#ifdef MACE_ENABLE_HEXAGON
#include "mace/core/runtime/hexagon/hexagon_control_wrapper.h"
#endif  // MACE_ENABLE_HEXAGON

namespace mace {

// Mace Tensor
class MaceTensor::Impl {
 public:
  std::vector<int64_t> shape;
  std::shared_ptr<float> data;
};

MaceTensor::MaceTensor(const std::vector<int64_t> &shape,
                       std::shared_ptr<float> data) {
  MACE_CHECK_NOTNULL(data.get());
  impl_ = std::unique_ptr<MaceTensor::Impl>(new MaceTensor::Impl());
  impl_->shape = shape;
  impl_->data = data;
}

MaceTensor::MaceTensor() {
  impl_ = std::unique_ptr<MaceTensor::Impl>(new MaceTensor::Impl());
}

MaceTensor::MaceTensor(const MaceTensor &other) {
  impl_ = std::unique_ptr<MaceTensor::Impl>(new MaceTensor::Impl());
  impl_->shape = other.shape();
  impl_->data = other.data();
}

MaceTensor::MaceTensor(const MaceTensor &&other) {
  impl_ = std::unique_ptr<MaceTensor::Impl>(new MaceTensor::Impl());
  impl_->shape = other.shape();
  impl_->data = other.data();
}

MaceTensor &MaceTensor::operator=(const MaceTensor &other) {
  impl_->shape = other.shape();
  impl_->data = other.data();
  return *this;
}

MaceTensor &MaceTensor::operator=(const MaceTensor &&other) {
  impl_->shape = other.shape();
  impl_->data = other.data();
  return *this;
}

MaceTensor::~MaceTensor() = default;

const std::vector<int64_t> &MaceTensor::shape() const { return impl_->shape; }

const std::shared_ptr<float> MaceTensor::data() const { return impl_->data; }

std::shared_ptr<float> MaceTensor::data() { return impl_->data; }

// Mace Engine
class MaceEngine::Impl {
 public:
  explicit Impl(DeviceType device_type);

  ~Impl();

  MaceStatus Init(const NetDef *net_def,
                  const std::vector<std::string> &input_nodes,
                  const std::vector<std::string> &output_nodes,
                  const unsigned char *model_data);

  MaceStatus Run(const std::map<std::string, MaceTensor> &inputs,
                 std::map<std::string, MaceTensor> *outputs,
                 RunMetadata *run_metadata);

 private:
  std::shared_ptr<OperatorRegistry> op_registry_;
  DeviceType device_type_;
  std::unique_ptr<Workspace> ws_;
  std::unique_ptr<NetBase> net_;
  std::map<std::string, mace::InputInfo> input_info_map_;
  std::map<std::string, mace::OutputInfo> output_info_map_;
#ifdef MACE_ENABLE_HEXAGON
  std::unique_ptr<HexagonControlWrapper> hexagon_controller_;
#endif

  MACE_DISABLE_COPY_AND_ASSIGN(Impl);
};

MaceEngine::Impl::Impl(DeviceType device_type)
    : op_registry_(new OperatorRegistry()),
      device_type_(device_type),
      ws_(new Workspace()),
      net_(nullptr)
#ifdef MACE_ENABLE_HEXAGON
      , hexagon_controller_(nullptr)
#endif
{
  LOG(INFO) << "Creating MaceEngine, MACE version: " << MaceVersion();
}

MaceStatus MaceEngine::Impl::Init(
    const NetDef *net_def,
    const std::vector<std::string> &input_nodes,
    const std::vector<std::string> &output_nodes,
    const unsigned char *model_data) {
  LOG(INFO) << "Initializing MaceEngine";
  // Get input and output information.
  for (auto &input_info : net_def->input_info()) {
    input_info_map_[input_info.name()] = input_info;
  }
  for (auto &output_info : net_def->output_info()) {
    output_info_map_[output_info.name()] = output_info;
  }
  // Set storage path for internal usage
  for (auto input_name : input_nodes) {
    if (input_info_map_.find(input_name) == input_info_map_.end()) {
      LOG(FATAL) << "'" << input_name
                 << "' is not belong to model's inputs: "
                 << MakeString(MapKeys(input_info_map_));
    }
    ws_->CreateTensor(MakeString("mace_input_node_", input_name),
                      GetDeviceAllocator(device_type_), DT_FLOAT);
  }
  for (auto output_name : output_nodes) {
    if (output_info_map_.find(output_name) == output_info_map_.end()) {
      LOG(FATAL) << "'" << output_name
                 << "' is not belong to model's outputs "
                 << MakeString(MapKeys(output_info_map_));
    }
    ws_->CreateTensor(MakeString("mace_output_node_", output_name),
                      GetDeviceAllocator(device_type_), DT_FLOAT);
  }
#ifdef MACE_ENABLE_HEXAGON
  if (device_type_ == HEXAGON) {
    hexagon_controller_.reset(new HexagonControlWrapper());
    MACE_CHECK(hexagon_controller_->Config(), "hexagon config error");
    MACE_CHECK(hexagon_controller_->Init(), "hexagon init error");
    hexagon_controller_->SetDebugLevel(
        static_cast<int>(mace::logging::LogMessage::MinVLogLevel()));
    int dsp_mode =
        ProtoArgHelper::GetOptionalArg<NetDef, int>(*net_def, "dsp_mode", 0);
    hexagon_controller_->SetGraphMode(dsp_mode);
    MACE_CHECK(hexagon_controller_->SetupGraph(*net_def, model_data),
               "hexagon setup graph error");
    if (VLOG_IS_ON(2)) {
      hexagon_controller_->PrintGraph();
    }
  } else {
#endif
    MACE_RETURN_IF_ERROR(ws_->LoadModelTensor(
        *net_def, device_type_, model_data));

    // Init model
    auto net = CreateNet(op_registry_, *net_def, ws_.get(), device_type_,
                         NetMode::INIT);
    MACE_RETURN_IF_ERROR(net->Run());
    net_ = CreateNet(op_registry_, *net_def, ws_.get(), device_type_);
#ifdef MACE_ENABLE_HEXAGON
  }
#endif
  return MaceStatus::MACE_SUCCESS;
}

MaceEngine::Impl::~Impl() {
  LOG(INFO) << "Destroying MaceEngine";
#ifdef MACE_ENABLE_HEXAGON
  if (device_type_ == HEXAGON) {
    if (VLOG_IS_ON(2)) {
      hexagon_controller_->GetPerfInfo();
      hexagon_controller_->PrintLog();
    }
    MACE_CHECK(hexagon_controller_->TeardownGraph(), "hexagon teardown error");
    MACE_CHECK(hexagon_controller_->Finalize(), "hexagon finalize error");
  }
#endif
}

MaceStatus MaceEngine::Impl::Run(
    const std::map<std::string, MaceTensor> &inputs,
    std::map<std::string, MaceTensor> *outputs,
    RunMetadata *run_metadata) {
  MACE_CHECK_NOTNULL(outputs);
  std::vector<Tensor *> input_tensors;
  std::vector<Tensor *> output_tensors;
  for (auto &input : inputs) {
    if (input_info_map_.find(input.first) == input_info_map_.end()) {
      LOG(FATAL) << "'" << input.first
                 << "' is not belong to model's inputs: "
                 << MakeString(MapKeys(input_info_map_));
    }
    Tensor *input_tensor =
        ws_->GetTensor(MakeString("mace_input_node_", input.first));
    MACE_RETURN_IF_ERROR(input_tensor->Resize(input.second.shape()));
    {
      Tensor::MappingGuard input_guard(input_tensor);
      float *input_data = input_tensor->mutable_data<float>();
      memcpy(input_data, input.second.data().get(),
             input_tensor->size() * sizeof(float));
    }
    input_tensors.push_back(input_tensor);
  }
  for (auto &output : *outputs) {
    if (output_info_map_.find(output.first) == output_info_map_.end()) {
      LOG(FATAL) << "'" << output.first
                 << "' is not belong to model's outputs: "
                 << MakeString(MapKeys(output_info_map_));
    }
    Tensor *output_tensor =
        ws_->GetTensor(MakeString("mace_output_node_", output.first));
    output_tensors.push_back(output_tensor);
  }
#ifdef MACE_ENABLE_HEXAGON
  if (device_type_ == HEXAGON) {
    MACE_CHECK(input_tensors.size() == 1 && output_tensors.size() == 1,
               "HEXAGON not support multiple inputs and outputs yet.");
    hexagon_controller_->ExecuteGraph(*input_tensors[0], output_tensors[0]);
  } else {
#endif
    MACE_RETURN_IF_ERROR(net_->Run(run_metadata));
#ifdef MACE_ENABLE_HEXAGON
  }
#endif

#ifdef MACE_ENABLE_OPENCL
  if (device_type_ == GPU) {
    OpenCLRuntime::Global()->SaveBuiltCLProgram();
  }
#endif
  for (auto &output : *outputs) {
    Tensor *output_tensor =
        ws_->GetTensor(MakeString("mace_output_node_", output.first));
    // save output
    if (output_tensor != nullptr && output.second.data() != nullptr) {
      Tensor::MappingGuard output_guard(output_tensor);
      auto shape = output_tensor->shape();
      int64_t output_size = std::accumulate(shape.begin(), shape.end(), 1,
                                            std::multiplies<int64_t>());
      MACE_CHECK(!shape.empty()) << "Output's shape must greater than 0";
      MACE_CHECK(shape == output.second.shape())
          << "Output shape mismatch: "
          << MakeString<int64_t>(output.second.shape())
          << " != " << MakeString<int64_t>(shape);
      std::memcpy(output.second.data().get(), output_tensor->data<float>(),
                  output_size * sizeof(float));
    } else {
      return MACE_INVALID_ARGS;
    }
  }
  return MACE_SUCCESS;
}

MaceEngine::MaceEngine(DeviceType device_type):
    impl_(new MaceEngine::Impl(device_type)) {}

MaceEngine::~MaceEngine() = default;

MaceStatus MaceEngine::Init(const NetDef *net_def,
                            const std::vector<std::string> &input_nodes,
                            const std::vector<std::string> &output_nodes,
                            const unsigned char *model_data) {
  return impl_->Init(net_def, input_nodes, output_nodes, model_data);
}

MaceStatus MaceEngine::Run(const std::map<std::string, MaceTensor> &inputs,
                           std::map<std::string, MaceTensor> *outputs,
                           RunMetadata *run_metadata) {
  return impl_->Run(inputs, outputs, run_metadata);
}

MaceStatus MaceEngine::Run(const std::map<std::string, MaceTensor> &inputs,
                           std::map<std::string, MaceTensor> *outputs) {
  return impl_->Run(inputs, outputs, nullptr);
}

const unsigned char *LoadModelData(const std::string &model_data_file,
                                   const size_t &data_size) {
  std::ifstream fd(model_data_file.c_str(), std::ios::in | std::ios::binary);
  size_t length;
  fd.seekg(0, std::ios::end);
  length = fd.tellg();
  fd.seekg(0, std::ios::beg);

  unsigned char* model_data = new unsigned char[data_size];

  MACE_CHECK(data_size <= length, "error model file size");

  fd.read((char*)model_data, data_size);

  //int fd = open(model_data_file.c_str(), O_RDONLY);
  //MACE_CHECK(fd >= 0, "Failed to open model data file ",
  //           model_data_file, ", error code: ", strerror(errno));

  //const unsigned char *model_data = static_cast<const unsigned char *>(
  //    mmap(nullptr, data_size, PROT_READ, MAP_PRIVATE, fd, 0));
  //MACE_CHECK(model_data != MAP_FAILED, "Failed to map model data file ",
  //           model_data_file, ", error code: ", strerror(errno));

  //int ret = close(fd);
  //MACE_CHECK(ret == 0, "Failed to close model data file ",
  //           model_data_file, ", error code: ", strerror(errno));

  return model_data;
}

void UnloadModelData(const unsigned char *model_data,
                     const size_t &data_size) {
  delete[] model_data;

  //int ret = munmap(const_cast<unsigned char *>(model_data),
  //                 data_size);
  //MACE_CHECK(ret == 0, "Failed to unmap model data file, error code: ",
  //           strerror(errno));
}

MaceStatus CreateMaceEngineFromProto(
    const std::vector<unsigned char> &model_pb,
    const std::string &model_data_file,
    const std::vector<std::string> &input_nodes,
    const std::vector<std::string> &output_nodes,
    const DeviceType device_type,
    std::shared_ptr<MaceEngine> *engine) {
  LOG(INFO) << "Create MaceEngine from model pb";
  // load model
  if (engine == nullptr) {
    return MaceStatus::MACE_INVALID_ARGS;
  }

  std::shared_ptr<NetDef> net_def(new NetDef());
  net_def->ParseFromArray(&model_pb[0], model_pb.size());

  index_t model_data_size = 0;
  for (auto &const_tensor : net_def->tensors()) {
    model_data_size = std::max(
        model_data_size,
        static_cast<index_t>(const_tensor.offset() +
                             const_tensor.data_size() *
                             GetEnumTypeSize(const_tensor.data_type())));
  }

  MaceStatus status;
  const unsigned char *model_data = nullptr;
  model_data = LoadModelData(model_data_file, model_data_size);

  engine->reset(new mace::MaceEngine(device_type));
  status = (*engine)->Init(
      net_def.get(), input_nodes, output_nodes, model_data);

  if (device_type == DeviceType::GPU || device_type == DeviceType::HEXAGON) {
    UnloadModelData(model_data, model_data_size);
  }
  return status;
}

}  // namespace mace
