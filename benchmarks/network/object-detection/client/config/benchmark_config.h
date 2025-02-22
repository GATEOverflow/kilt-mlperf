//
// MIT License
//
// Copyright (c) 2021 - 2023 Krai Ltd
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.POSSIBILITY OF SUCH DAMAGE.
//

#ifndef BENCHMARK_CONFIG_H
#define BENCHMARK_CONFIG_H

#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <assert.h>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string.h>
#include <vector>

#include "config/config_tools/config_tools.h"
#include "iconfig.h"

namespace KRAI {

template <char delimiter> class WordDelimitedBy : public std::string {};

template <char delimiter>
std::istream &operator>>(std::istream &is, WordDelimitedBy<delimiter> &output) {
  std::getline(is, output, delimiter);
  return is;
}

//----------------------------------------------------------------------

class ObjectDetectionDataSourceConfig : public IDataSourceConfig {

public:
  virtual const int getImageSize() const { return image_size; };
  virtual const int getNumChannels() const { return num_channels; };

  virtual const std::vector<std::string> &getListOfImageFilenames() const {
    return _available_image_list;
  };
  virtual const std::string &getDatasetDir() const { return images_dir; };

  virtual const int getMaxImagesInMemory() const {
    return images_in_memory_max;
  };

  ObjectDetectionDataSourceConfig() {

    assert(image_size_height == image_size_width);
    image_size = image_size_height;

    // Load list of images to be processed
    std::ifstream file(images_dir + "/" + available_images_file);
    if (!file)
      throw "Unable to open image list file " + available_images_file;

    for (std::string s; !getline(file, s).fail();) {
      std::istringstream iss(s);
      std::vector<std::string> row(
          (std::istream_iterator<WordDelimitedBy<';'>>(iss)),
          std::istream_iterator<WordDelimitedBy<';'>>());
      _available_image_list.emplace_back(row[0]);
    }
  }

private:
  const int image_size_height =
      getconfig_i("KILT_DATASET_OBJECT_DETECTION_IMAGE_HEIGHT");
  const int image_size_width =
      getconfig_i("KILT_DATASET_OBJECT_DETECTION_IMAGE_WIDTH");
  int image_size;

  const int num_channels = alter_str_i(
      getconfig_c("KILT_DATASET_OBJECT_DETECTION_IMAGE_CHANNELS"), 3);

  const std::string images_dir =
      getconfig_s("KILT_DATASET_OBJECT_DETECTION_PREPROCESSED_DIR");

  const std::string available_images_file =
      getconfig_s("KILT_DATASET_OBJECT_DETECTION_PREPROCESSED_SUBSET_FOF");

  const int images_in_memory_max = getconfig_i("LOADGEN_BUFFER_SIZE");

  const int qaic_batch_size = getconfig_i("KILT_MODEL_BATCH_SIZE");

  std::vector<std::string> _available_image_list;
};

IDataSourceConfig *getDataSourceConfig() {
  return new ObjectDetectionDataSourceConfig();
};

class ModelConfig : public IModelConfig {
public:
  virtual ~ModelConfig(){};

  virtual const IO_TYPE getInputDatatype(const int buf_idx) const {
    if (strcmp(qaic_skip_stage.c_str(), "convert") == 0) {
      return INT8;
    } else {
      return FLOAT32;
    }
  }

  virtual const IO_TYPE getOutputDatatype(const int buf_idx) const {
    return INT32; // never used
  }

  const int getMaxDetections() { return max_detections; }
  const bool disableNMS() { return disable_nms; }
  const std::string getPriorsBinPath() { return priors_bin_path; }

private:
  std::string qaic_skip_stage =
      alter_str(getconfig_c("KILT_DEVICE_QAIC_SKIP_STAGE"), std::string(""));

  const std::string priors_bin_path =
      getconfig_s("KILT_MODEL_NMS_PRIOR_BIN_PATH");
  const int max_detections = getconfig_i("KILT_MODEL_NMS_MAX_DETECTIONS");
  const bool disable_nms = (getconfig_c("KILT_MODEL_NMS_DISABLE") != NULL);
};

IModelConfig *getModelConfig() { return new ModelConfig(); }

IDeviceConfig *getDeviceConfig() { return nullptr; }

IServerConfig *getServerConfig() { return nullptr; }

class NetworkClientConfig {
public:
  const std::string getNetworkServerIPAddress() { return server_ip_address; }

  const int getNetworkServerPort() { return server_port; }

  const int getVerbosityLevel() { return verbosity_level; }

  const int getNumSockets() { return num_sockets; }

  const int num_sockets = getconfig_i("KILT_NETWORK_NUM_SOCKETS");

  const int verbosity_level = getconfig_i("KILT_VERBOSE");

  const std::string localhost = "127.0.0.1";
  const std::string server_ip_address =
      alter_str(getconfig_c("KILT_NETWORK_SERVER_IP_ADDRESS"), localhost);

  const int server_port =
      alter_str_i(getconfig_c("KILT_NETWORK_SERVER_PORT"), 8080);
};

}; // namespace KRAI
#endif
