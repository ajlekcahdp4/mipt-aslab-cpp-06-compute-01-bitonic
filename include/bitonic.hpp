/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <tsimmerman.ss@phystech.edu>, <alex.rom23@mail.ru> wrote this file.  As long as you
 * retain this notice you can do whatever you want with this stuff. If we meet
 * some day, and you think this stuff is worth it, you can buy me a beer in
 * return.
 * ----------------------------------------------------------------------------
 */

#pragma once

#include "opencl_include.hpp"
#include "selector.hpp"
#include "utils.hpp"

#include <bit>
#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace bitonic {

template <typename T> struct i_bitonic_sort {
  using size_type = unsigned;
  void sort(std::span<T> container, clutils::profiling_info *time = nullptr) { return operator()(container, time); }
  virtual void operator()(std::span<T>, clutils::profiling_info *) = 0;
  virtual ~i_bitonic_sort() {}
};

template <typename T> struct cpu_bitonic_sort : public i_bitonic_sort<T> {
  using typename i_bitonic_sort<T>::size_type;

  void operator()(std::span<T> container, clutils::profiling_info *info) override {
    size_type size = container.size();
    if (std::popcount(size) != 1 || size < 2) throw std::runtime_error{"Only power-of-two sequences are supported"};

    const auto execute_step = [container, size](size_type stage, size_type step) {
      const size_type part_length = 1 << (step + 1);

      const auto calc_j = [stage, step, part_length](auto i) -> size_type {
        if (stage == step) return part_length - i - 1;
        return i + part_length / 2;
      };

      for (size_type k = 0; k < size / part_length; ++k) {
        for (size_type i = 0; i < part_length / 2; ++i) {
          const auto j = calc_j(i);
          auto &first = container[k * part_length + i];
          auto &second = container[k * part_length + j];
          if (first > second) std::swap(first, second);
        }
      }
    };

    const auto wall_start = std::chrono::high_resolution_clock::now();

    size_type stages = std::countr_zero(size);
    for (size_type stage = 0; stage < stages; ++stage) {
      for (size_type temp = 0, step = stage; temp <= stage; step = stage - (++temp)) {
        execute_step(stage, step);
      }
    }

    const auto wall_end = std::chrono::high_resolution_clock::now();

    if (info) info->wall = info->pure = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start);
  }
};

template <typename T> class gpu_bitonic : public i_bitonic_sort<T>, protected clutils::platform_selector {
protected:
  cl::Context m_ctx;
  cl::CommandQueue m_queue;
  using typename i_bitonic_sort<T>::size_type;

  static constexpr clutils::platform_version cl_api_version = {2, 2};

  gpu_bitonic()
      : clutils::platform_selector{cl_api_version}, m_ctx{m_device}, m_queue{m_ctx, cl::QueueProperties::Profiling} {}

  using func_signature = cl::Event(cl::Buffer);

  void run_boilerplate(std::span<T> container, std::function<func_signature> func) {
    cl::Buffer buff = {m_ctx, CL_MEM_READ_WRITE, clutils::sizeof_container(container)};
    cl::copy(m_queue, container.begin(), container.end(), buff);

    auto event = func(buff);
    event.wait();

    cl::copy(m_queue, buff, container.begin(), container.end());
  }
};

template <typename T, typename t_name> class naive_bitonic : public gpu_bitonic<T> {
  struct kernel {
    using functor_type = cl::KernelFunctor<cl::Buffer, int, int>;
    static std::string source(std::string type) {
      static const std::string naive_source = R"(
      __kernel void naive_bitonic (__global TYPE *buff, int step, int stage) {
        int i = get_global_id(0);
        int seq_len = 1 << (stage + 1);
        int power_of_two = 1 << (step - stage);
        int seq_n = i / seq_len;
        int odd = seq_n / power_of_two;
        bool increasing = ((odd % 2) == 0);
        int halflen = seq_len / 2;
        if (i < (seq_len * seq_n) + halflen) {
          int   j = i + halflen;
          if (((buff[i] > buff[j]) && increasing) ||
              ((buff[i] < buff[j]) && !increasing)) {
            TYPE tmp = buff[i];
            buff[i] = buff[j];
            buff[j] = tmp;
          }
        }
      })";

      auto type_macro_def = clutils::kernel_define("TYPE", type);
      return type_macro_def + naive_source;
    }

    static std::string entry() { return "naive_bitonic"; }
  };

private:
  cl::Program m_program;

  typename kernel::functor_type m_functor;

  using gpu_bitonic<T>::m_queue;
  using gpu_bitonic<T>::m_ctx;
  using gpu_bitonic<T>::run_boilerplate;

  using typename gpu_bitonic<T>::size_type;

public:
  naive_bitonic()
      : gpu_bitonic<T>{}, m_program{m_ctx, kernel::source(t_name::name_str), true}, m_functor{m_program,
                                                                                              kernel::entry()} {}

  void operator()(std::span<T> container, clutils::profiling_info *time = nullptr) override {
    const size_type size = container.size(), stages = std::countr_zero(size);
    if (std::popcount(size) != 1 || size < 2) throw std::runtime_error("Only power-of-two sequences are supported");
    cl::Event prev_event, first_event;

    auto submit = [&, first_iter = true, size = container.size()](auto buf, auto stage, auto step) mutable {
      if (first_iter) {
        first_iter = false;
        auto args = cl::EnqueueArgs{m_queue, size};
        first_event = prev_event = m_functor(args, buf, stage, step);
      } else {
        auto args = cl::EnqueueArgs{m_queue, prev_event, size};
        prev_event = m_functor(args, buf, stage, step);
      }
    };

    const auto func = [&, stages](auto buf) {
      for (unsigned stage = 0; stage < stages; ++stage) {
        for (int step = stage; step >= 0; --step) {
          submit(buf, stage, step);
        }
      }
      return prev_event;
    };

    auto wall_start = std::chrono::high_resolution_clock::now();
    run_boilerplate(container, func);
    auto wall_end = std::chrono::high_resolution_clock::now();

    std::chrono::nanoseconds pure_start{first_event.getProfilingInfo<CL_PROFILING_COMMAND_START>()},
        pure_end{prev_event.getProfilingInfo<CL_PROFILING_COMMAND_END>()};

    if (time) {
      time->wall = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start);
      time->pure = std::chrono::duration_cast<std::chrono::milliseconds>(pure_end - pure_start);
    }
  }
};

template <typename T, typename t_name> class local_bitonic : public gpu_bitonic<T> {
  struct kernel_presort {
    using functor_type = cl::KernelFunctor<cl::Buffer, int, int>;
    static std::string source(std::string type, unsigned local_size) {
      static const std::string naive_source = R"(
      __kernel void local_presort (__global TYPE *buff, int step_start, int step_end) {
        int global_i = get_global_id(0);
        int local_i = get_local_id(0);
        __local segment [SEGMENT_SIZE];
        segment[local_i] = buff[global_i];
        barrier(CLK_LOCAL_MEM_FENCE);
        const int i = local_i;
        for (int step = step_start; step < step_end; ++step) {
          for (int stage = step; stage >=0 ; --stage) {
            int seq_len = 1 << (stage + 1);
            int power_of_two = 1 << (step - stage);
            int seq_n = i / seq_len;

            // direction determined by global position, not local
            int odd = (global_i / seq_len) / power_of_two;
            bool increasing = ((odd % 2) == 0);
            int halflen = seq_len / 2;

            if (i < (seq_len * seq_n) + halflen){
              int   j = i + halflen;
              if (((segment[i] > segment[j]) && increasing) ||
                  ((segment[i] < segment[j]) && !increasing)) {
                TYPE tmp = segment[i];
                segment[i] = segment[j];
                segment[j] = tmp;
              }
            }
            barrier(CLK_LOCAL_MEM_FENCE);
          }
        }
        buff[global_i] = segment[local_i];
      })";

      auto type_macro_def = clutils::kernel_define("TYPE", type);
      auto local_size_macro_def = clutils::kernel_define("SEGMENT_SIZE", local_size);
      return type_macro_def + local_size_macro_def + naive_source;
    }

    static std::string entry() { return "local_presort"; }
  };

private:
  cl::Program m_program;
  typename kernel_presort::functor_type m_functor;
  using gpu_bitonic<T>::m_queue;
  using gpu_bitonic<T>::run_boilerplate;

  unsigned m_local_size{};

public:
  local_bitonic(const unsigned segment_size)
      : gpu_bitonic<T>{}, m_program{gpu_bitonic<T>::m_ctx, kernel_presort::source(t_name::name_str, segment_size),
                                    true},
        m_functor{m_program, kernel_presort::entry()}, m_local_size{segment_size} {}

  void operator()(std::span<T> container, clutils::profiling_info *time = nullptr) override {
    unsigned size = container.size(), steps_n = std::countr_zero(size), nfst = std::countr_zero(m_local_size);
    if (std::popcount(size) != 1 || size < 2) throw std::runtime_error("Only power-of-two sequences are supported");
    cl::Event prev_event, first_event;

    auto submit = [&, first_iter = true, size = container.size()](auto buf) mutable {
      auto args = cl::EnqueueArgs{m_queue, size, m_local_size};
      first_event = prev_event = m_functor(args, buf, 0, std::min(nfst - 1, steps_n));
    };

    const auto func = [&](auto buf) {
      submit(buf);
      return prev_event;
    };

    auto wall_start = std::chrono::high_resolution_clock::now();
    run_boilerplate(container, func);
    auto wall_end = std::chrono::high_resolution_clock::now();

    std::chrono::nanoseconds pure_start{first_event.getProfilingInfo<CL_PROFILING_COMMAND_START>()},
        pure_end{prev_event.getProfilingInfo<CL_PROFILING_COMMAND_END>()};

    if (time) {
      time->wall = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start);
      time->pure = std::chrono::duration_cast<std::chrono::milliseconds>(pure_end - pure_start);
    }
  }
};

} // namespace bitonic