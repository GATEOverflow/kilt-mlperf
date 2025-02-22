//
// MIT License
//
// Copyright (c) 2023 - 2023 Krai Ltd
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
#include <sstream>
#include <string.h>
#include <vector>

#include "config/config_tools/config_tools.h"
#include "iconfig.h"

namespace KRAI {

//----------------------------------------------------------------------

class SquadDataSourceConfig : public IDataSourceConfig {

public:
  const std::string getInputIDs() { return input_ids; }

  const std::string getInputMask() { return input_mask; }

  const std::string getSegmentIDs() { return segment_ids; }

  const int getDatasetSize() { return dataset_size; }

  const int getBufferSize() { return inputs_in_memory_max; }

  const int getDataSourceSequenceLength() { return max_seq_length; }

  const int getDeviceBatchSize() { return device_batch_size; }

  const std::string getLoadgenScenario() { return loadgen_scenario; }

private:
  const std::string squad_dataset_tokenized_path =
      getconfig_s("KILT_DATASET_SQUAD_TOKENIZED_ROOT");

  const std::string input_ids =
      squad_dataset_tokenized_path + "/" +
      getconfig_s("KILT_DATASET_SQUAD_TOKENIZED_INPUT_IDS");
  const std::string input_mask =
      squad_dataset_tokenized_path + "/" +
      getconfig_s("KILT_DATASET_SQUAD_TOKENIZED_INPUT_MASK");
  const std::string segment_ids =
      squad_dataset_tokenized_path + "/" +
      getconfig_s("KILT_DATASET_SQUAD_TOKENIZED_SEGMENT_IDS");

  const int max_seq_length =
      getconfig_i("KILT_DATASET_SQUAD_TOKENIZED_MAX_SEQ_LENGTH");

  const int device_batch_size = getconfig_i("KILT_DEVICE_TENSORRT_BATCH_SIZE");

  const int inputs_in_memory_max = getconfig_i("LOADGEN_BUFFER_SIZE");
  const int dataset_size = getconfig_i("LOADGEN_DATASET_SIZE");

  const std::string loadgen_scenario = getconfig_s("LOADGEN_SCENARIO");
};

IDataSourceConfig *getDataSourceConfig() { return new SquadDataSourceConfig(); }

class BertModelConfig : public IModelConfig {

public:
  enum BERT_MODEL_VARIANT { BERT_ORIG, BERT_PACKED, DISTILBERT_PACKED };

  BertModelConfig() {

    std::string kilt_model_variant_string =
        alter_str(getconfig_c("KILT_MODEL_BERT_VARIANT"), std::string("none"));

    if (kilt_model_variant_string == "BERT_ORIG")
      bert_model_variant = BERT_ORIG;
    else if (kilt_model_variant_string == "BERT_PACKED")
      bert_model_variant = BERT_PACKED;
    else if (kilt_model_variant_string == "DISTILBERT_PACKED")
      bert_model_variant = DISTILBERT_PACKED;
    else
      bert_model_variant = BERT_ORIG; // default to BERT PACKED
  }

  virtual const BERT_MODEL_VARIANT getModelVariant() const {
    return bert_model_variant;
  }

  int getModelSequenceLength() const { return model_packed_seq_len; }

  const std::string getEngineSource() { return engine_source; }

private:
  std::string qaic_skip_stage =
      alter_str(getconfig_c("KILT_DEVICE_QAIC_SKIP_STAGE"), std::string(""));

  std::string kilt_device_name =
      alter_str(getconfig_c("KILT_DEVICE_NAME"), std::string("none"));

  const int model_packed_seq_len =
      alter_str_i(getconfig_c("KILT_MODEL_BERT_SEQ_LENGTH"), 384);

  BERT_MODEL_VARIANT bert_model_variant;
  const std::string engine_source =
      getconfig_s("KILT_DEVICE_TENSORRT_ENGINE_SOURCE");
};

IModelConfig *getModelConfig() { return new BertModelConfig(); }

}; // namespace KRAI
#endif
