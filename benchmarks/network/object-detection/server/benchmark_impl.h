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

#ifndef BENCHMARK_IMPL_H
#define BENCHMARK_IMPL_H

#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "kilt_impl.h"
#include "sample.h"

#include "config/benchmark_config.h"

#include "plugins/nms-abp/nms_abp.h"

#if defined(__amd64__) && defined(ENABLE_ZEN2)
#include <cstdint>
#include <immintrin.h>
#endif

#if defined(MODEL_R34)
#define Model_Params R34_Params
#elif defined(MODEL_RX50)
#define Model_Params RX50_Params
#else
#define Model_Params MV1_Params
#endif

#define DEBUG(msg) std::cout << "DEBUG: " << msg << std::endl;

namespace KRAI {

template <typename TData> class StaticBuffer {
public:
  StaticBuffer(const int size, TData *buffer = nullptr) : _size(size) {
    if (buffer != nullptr)
      _buffer = buffer;
    else
      _buffer = (TData *)aligned_alloc(32, size * sizeof(TData));
  }

  virtual ~StaticBuffer() { free(_buffer); }

  TData *data() const { return _buffer; }
  int size() const { return _size; }

protected:
  const int _size;
  TData *_buffer;
};

template <typename TData> class SampleData : public StaticBuffer<TData> {
public:
  SampleData(const int s, TData *buffer = nullptr)
      : StaticBuffer<TData>(s, buffer) {}

  virtual void load(const std::string &path, int vl) {

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file)
      throw "Failed to open image data " + path;
    file.read(reinterpret_cast<char *>(this->_buffer), this->_size);
    if (vl > 1) {
      std::cout << "Loaded file: " << path << std::endl;
    } else if (vl) {
      std::cout << 'l' << std::flush;
    }
  }
};

class ResultData {
public:
  ResultData(const IConfig *c) : _size(0) {
    _buffer = new float
        [(static_cast<ModelConfig *>(c->model_cfg)->getMaxDetections() + 1) *
         7];
  }
  ~ResultData() { delete[] _buffer; }
  int size() const { return _size; }
  void set_size(int size) { _size = size; }
  float *data() const { return _buffer; }

private:
  float *_buffer;
  int _size;
};

class WorkingBuffers {

public:
  WorkingBuffers(const IConfig *c) {
    cfg = c;
    for (int i = 0; i < cfg->model_cfg->getBatchSize(); ++i) {
      nms_results.push_back(std::vector<std::vector<float>>(
          0, std::vector<float>(NUM_COORDINATES + 2, 0)));
      reformatted_results.push_back(new ResultData(c));
    }
  }

  ~WorkingBuffers() {
    for (int i = 0; i < cfg->model_cfg->getBatchSize(); ++i)
      delete reformatted_results[i];
  }

  void reset() {
    for (int i = 0; i < cfg->model_cfg->getBatchSize(); ++i)
      nms_results[i].clear();
  }

  std::vector<std::vector<std::vector<float>>> nms_results;
  std::vector<ResultData *> reformatted_results;
  const IConfig *cfg;
};

template <typename TInputDataType, typename TOutput1DataType,
          typename TOutput2DataType>
class ObjectDetectionModel : public IModel {
public:
  ObjectDetectionModel(const IConfig *config) : _config(config) {

    datasource_cfg =
        static_cast<ObjectDetectionDataSourceConfig *>(_config->datasource_cfg);
    model_cfg = static_cast<ModelConfig *>(_config->model_cfg);

    nms_abp_processor =
        new NMS_ABP<TOutput1DataType, TOutput2DataType, Model_Params>(
            model_cfg->getPriorsBinPath());
  }

  void configureWorkload(IDataSource *data_source, const void *samples,
                         std::vector<void *> &in_ptrs) override {

    const std::vector<Sample> *s =
        reinterpret_cast<const std::vector<Sample> *>(samples);

    uint32_t buf_size = datasource_cfg->getImageSize() *
                        datasource_cfg->getImageSize() *
                        datasource_cfg->getNumChannels();

    for (int i = 0; i < s->size(); ++i) {

      TInputDataType *src_ptr = reinterpret_cast<TInputDataType *>((*s)[i].buf);

      TInputDataType *dest_ptr =
          reinterpret_cast<TInputDataType *>(in_ptrs[0]) + i * buf_size;

#if defined(__amd64__) && defined(ENABLE_ZEN2)
#if !defined(MODEL_R34) && !defined(MODEL_RX50)
      __m128i *src = reinterpret_cast<__m128i *>(src_ptr);
      __m128i *dest = reinterpret_cast<__m128i *>(dest_ptr);
      int64_t vectors = buf_size / sizeof(*src);
      for (; vectors > 0; vectors--, src++, dest++) {
        const __m128i loaded = _mm_stream_load_si128(src);
        _mm_stream_si128(dest, loaded);
      }
#else // MODEL_MV1
      const __m256i *src = reinterpret_cast<const __m256i *>(src_ptr);
      __m256i *dest = reinterpret_cast<__m256i *>(dest_ptr);
      int64_t vectors = buf_size / sizeof(*src);
      for (; vectors > 0; vectors--, src++, dest++) {
        const __m256i loaded = _mm256_stream_load_si256(src);
        _mm256_stream_si256(dest, loaded);
      }
#endif
      unsigned rem = buf_size % sizeof(*src);
      if (rem > 0) {
        memcpy((uint8_t *)dest, (uint8_t *)src, rem);
      }
      _mm_sfence();
#else
      std::copy(src_ptr, src_ptr + buf_size, dest_ptr);
#endif
    }
  }

  void postprocessResults(void *samples, std::vector<void *> &out_ptrs) {

    std::vector<Sample> *s = reinterpret_cast<std::vector<Sample> *>(samples);

    // std::vector<mlperf::QuerySampleResponse> responses;

#if defined(MODEL_RX50)
    std::vector<TOutput1DataType *> boxes_ptrs(modelParams.OUTPUT_LEVELS);
    std::vector<TOutput2DataType *> classes_ptrs(modelParams.OUTPUT_LEVELS);
    std::vector<uint64_t *> topk_ptrs(modelParams.OUTPUT_LEVELS);
#else
    TOutput1DataType *boxes_ptr =
        (TOutput1DataType *)out_ptrs[modelParams.BOXES_INDEX];
    TOutput2DataType *classes_ptr =
        (TOutput2DataType *)out_ptrs[modelParams.CLASSES_INDEX];
#endif

    WorkingBuffers *wbs = popWorkingBuffers();

    for (int i = 0; i < s->size(); i++) {

      std::vector<std::vector<float>> &nms_res = wbs->nms_results[i];
      ResultData *next_result_ptr = wbs->reformatted_results[i];

      if (model_cfg->disableNMS()) {
        next_result_ptr->set_size(1 * 7);
        next_result_ptr->data()[0] = (float)((*s)[i].index);
      } else {
#if defined(MODEL_RX50)
        for (int g = 0; g < modelParams.OUTPUT_LEVELS; ++g) {
          TOutput1DataType *boxes_ptr =
              (TOutput1DataType *)out_ptrs[modelParams.BOXES_INDEX + g];
          TOutput2DataType *classes_ptr =
              (TOutput2DataType *)out_ptrs[modelParams.CLASSES_INDEX + g];
          uint64_t *topk_ptr = (uint64_t *)out_ptrs[modelParams.TOPK_INDEX + g];

          // std::cout << boxes_ptr[0] << " " << classes_ptr[0] << " " <<
          // topk_ptr << std::endl;

          boxes_ptrs[g] =
              boxes_ptr + i * modelParams.TOTAL_NUM_BOXES * NUM_COORDINATES;
          classes_ptrs[g] = classes_ptr + i * modelParams.TOTAL_NUM_BOXES;
          topk_ptrs[g] = topk_ptr + i * modelParams.TOTAL_NUM_BOXES;
        }
        nms_abp_processor->anchorBoxProcessing(
            (const TOutput1DataType **)boxes_ptrs.data(),
            (const TOutput2DataType **)classes_ptrs.data(),
            (const uint64_t **)topk_ptrs.data(), std::ref(nms_res),
            (float)((*s)[i].index));
#else // Classes + Boxes only
        TOutput1DataType *dataLoc =
            boxes_ptr + i * modelParams.TOTAL_NUM_BOXES * NUM_COORDINATES;
        TOutput2DataType *dataConf =
            classes_ptr +
            i * modelParams.TOTAL_NUM_BOXES * modelParams.NUM_CLASSES;
        nms_abp_processor->anchorBoxProcessing(
            dataLoc, dataConf, std::ref(nms_res), (float)((*s)[i].index));
#endif

        int num_elems = nms_res.size() < (model_cfg->getMaxDetections() + 1)
                            ? nms_res.size()
                            : (model_cfg->getMaxDetections() + 1);

        // std::cout << "num_elems: " << num_elems << " " <<
        // model_cfg->getMaxDetections()+1 << std::endl;

        next_result_ptr->set_size(num_elems * 7);
        float *buffer = next_result_ptr->data();

        for (int j = 0; j < num_elems; j++) {
          buffer[0] = nms_res[j][0];
          buffer[1] = nms_res[j][1];
          buffer[2] = nms_res[j][2];
          buffer[3] = nms_res[j][3];
          buffer[4] = nms_res[j][4];
          buffer[5] = nms_res[j][5];
          buffer[6] = nms_res[j][6];

          // std::cout << buffer[0] << "," << buffer[1] << "," << buffer[2] <<
          // "," << buffer[3] << "," << buffer[4] << "," << buffer[5] << "," <<
          // buffer[6] << std::endl;
          buffer += 7;
        }
      }
      (*s)[i].callback(&((*s)[i]), next_result_ptr->size(),
                       next_result_ptr->data());

      // responses.push_back({(*s)[i].id, uintptr_t(next_result_ptr->data()),
      //                      next_result_ptr->size() * sizeof(float) });
    }

    // mlperf::QuerySamplesComplete(responses.data(), responses.size());
    pushWorkingBuffers(wbs);
  }

private:
  const IConfig *_config;

  ObjectDetectionDataSourceConfig *datasource_cfg;
  ModelConfig *model_cfg;

  int _current_buffer_size = 0;
  // std::vector<std::vector<std::vector<float>>> *nms_results;
  // std::vector<ResultData*> *reformatted_results;

  NMS_ABP<TOutput1DataType, TOutput2DataType, Model_Params> *nms_abp_processor;
  Model_Params modelParams;

  WorkingBuffers *popWorkingBuffers() {
    working_buffs_mtx.lock();
    WorkingBuffers *tmp;
    if (working_buffers_list.size() == 0) {
      tmp = new WorkingBuffers(_config);
    } else {
      tmp = working_buffers_list.back();
      working_buffers_list.pop_back();
    }
    working_buffs_mtx.unlock();
    return tmp;
  }

  void pushWorkingBuffers(WorkingBuffers *bufs) {
    working_buffs_mtx.lock();
    bufs->reset();
    working_buffers_list.push_back(bufs);
    working_buffs_mtx.unlock();
  }

  std::vector<WorkingBuffers *> working_buffers_list;
  std::mutex working_buffs_mtx;
};

IModel *modelConstruct(IConfig *config) {

  if (config->model_cfg->getInputDatatype(0) == IModelConfig::IO_TYPE::FLOAT32)
    return new ObjectDetectionModel<float, float, float>(config);
  else
#if defined(MODEL_R34)
    return new ObjectDetectionModel<uint8_t, uint8_t, uint16_t>(config);
#elif defined(MODEL_RX50)
    return new ObjectDetectionModel<uint8_t, uint16_t, uint16_t>(config);
#else // MODEL_MV1
    return new ObjectDetectionModel<uint8_t, uint8_t, uint8_t>(config);
#endif
}

IDataSource *dataSourceConstruct(IConfig *config, std::vector<int> affinities) {

  return nullptr;
}

} // namespace KRAI

#endif // BENCHMARK_IMPL_H
