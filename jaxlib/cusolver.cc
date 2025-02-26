/* Copyright 2019 Google LLC

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

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "third_party/gpus/cuda/include/cusolverDn.h"
#include "include/pybind11/numpy.h"
#include "include/pybind11/pybind11.h"
#include "include/pybind11/stl.h"

namespace jax {
namespace {

namespace py = pybind11;

void ThrowIfError(cudaError_t error) {
  if (error != cudaSuccess) {
    throw std::runtime_error("CUDA operation failed");
  }
}

void ThrowIfErrorStatus(cusolverStatus_t status) {
  switch (status) {
    case CUSOLVER_STATUS_SUCCESS:
      return;
    case CUSOLVER_STATUS_NOT_INITIALIZED:
      throw std::runtime_error("cuSolver has not been initialized");
    case CUSOLVER_STATUS_ALLOC_FAILED:
      throw std::runtime_error("cuSolver allocation failed");
    case CUSOLVER_STATUS_INVALID_VALUE:
      throw std::runtime_error("cuSolver invalid value error");
    case CUSOLVER_STATUS_ARCH_MISMATCH:
      throw std::runtime_error("cuSolver architecture mismatch error");
    case CUSOLVER_STATUS_MAPPING_ERROR:
      throw std::runtime_error("cuSolver mapping error");
    case CUSOLVER_STATUS_EXECUTION_FAILED:
      throw std::runtime_error("cuSolver execution failed");
    case CUSOLVER_STATUS_INTERNAL_ERROR:
      throw std::runtime_error("cuSolver internal error");
    case CUSOLVER_STATUS_MATRIX_TYPE_NOT_SUPPORTED:
      throw std::invalid_argument("cuSolver matrix type not supported error");
    case CUSOLVER_STATUS_NOT_SUPPORTED:
      throw std::runtime_error("cuSolver not supported error");
    case CUSOLVER_STATUS_ZERO_PIVOT:
      throw std::runtime_error("cuSolver zero pivot error");
    case CUSOLVER_STATUS_INVALID_LICENSE:
      throw std::runtime_error("cuSolver invalid license error");
    default:
      throw std::runtime_error("Unknown cuSolver error");
  }
}

// To avoid creating cusolver contexts in the middle of execution, we maintain
// a pool of them.
class SolverHandlePool {
 public:
  SolverHandlePool() = default;

  // RAII class representing a cusolver handle borrowed from the pool. Returns
  // the handle to the pool on destruction.
  class Handle {
   public:
    Handle() = default;
    ~Handle() {
      if (pool_) {
        pool_->Return(handle_);
      }
    }

    Handle(Handle const&) = delete;
    Handle(Handle&& other) {
      pool_ = other.pool_;
      handle_ = other.handle_;
      other.pool_ = nullptr;
      other.handle_ = nullptr;
    }
    Handle& operator=(Handle const&) = delete;
    Handle& operator=(Handle&& other) {
      pool_ = other.pool_;
      handle_ = other.handle_;
      other.pool_ = nullptr;
      other.handle_ = nullptr;
      return *this;
    }

    cusolverDnHandle_t get() { return handle_; }

   private:
    friend class SolverHandlePool;
    Handle(SolverHandlePool* pool, cusolverDnHandle_t handle)
        : pool_(pool), handle_(handle) {}
    SolverHandlePool* pool_ = nullptr;
    cusolverDnHandle_t handle_ = nullptr;
  };

  // Borrows a handle from the pool. If 'stream' is non-null, sets the stream
  // associated with the handle.
  static Handle Borrow(cudaStream_t stream = nullptr);

 private:
  static SolverHandlePool* Instance();

  void Return(cusolverDnHandle_t handle);

  absl::Mutex mu_;
  std::vector<cusolverDnHandle_t> handles_ GUARDED_BY(mu_);
};

/*static*/ SolverHandlePool* SolverHandlePool::Instance() {
  static auto* pool = new SolverHandlePool;
  return pool;
}

/*static*/ SolverHandlePool::Handle SolverHandlePool::Borrow(
    cudaStream_t stream) {
  SolverHandlePool* pool = Instance();
  absl::MutexLock lock(&pool->mu_);
  cusolverDnHandle_t handle;
  if (pool->handles_.empty()) {
    ThrowIfErrorStatus(cusolverDnCreate(&handle));
  } else {
    handle = pool->handles_.back();
    pool->handles_.pop_back();
  }
  if (stream) {
    ThrowIfErrorStatus(cusolverDnSetStream(handle, stream));
  }
  return Handle(pool, handle);
}

void SolverHandlePool::Return(cusolverDnHandle_t handle) {
  absl::MutexLock lock(&mu_);
  handles_.push_back(handle);
}

// Set of types known to Cusolver.
enum class Type {
  F32,
  F64,
  C64,
  C128,
};

// Converts a NumPy dtype to a Type.
Type DtypeToType(const py::dtype& np_type) {
  static auto* types = new absl::flat_hash_map<std::pair<char, int>, Type>({
      {{'f', 4}, Type::F32},
      {{'f', 8}, Type::F64},
      {{'c', 8}, Type::C64},
      {{'c', 16}, Type::C128},
  });
  auto it = types->find({np_type.kind(), np_type.itemsize()});
  if (it == types->end()) {
    throw std::invalid_argument(
        absl::StrFormat("Unsupported dtype %s", py::repr(np_type)));
  }
  return it->second;
}

int SizeOfType(Type type) {
  switch (type) {
    case Type::F32:
      return sizeof(float);
    case Type::F64:
      return sizeof(double);
    case Type::C64:
      return sizeof(cuComplex);
    case Type::C128:
      return sizeof(cuDoubleComplex);
  }
}

// Descriptor objects are opaque host-side objects used to pass data from JAX
// to the custom kernel launched by XLA. Currently simply treat host-side
// structures as byte-strings; this is not portable across architectures. If
// portability is needed, we could switch to using a representation such as
// protocol buffers or flatbuffers.

// Packs a descriptor object into a py::bytes structure.
template <typename T>
py::bytes PackDescriptor(const T& descriptor) {
  return py::bytes(absl::bit_cast<const char*>(&descriptor), sizeof(T));
}

// Unpacks a descriptor object from a byte string.
template <typename T>
const T* UnpackDescriptor(const char* opaque, size_t opaque_len) {
  if (opaque_len != sizeof(T)) {
    throw std::runtime_error("Invalid size for linalg operation descriptor.");
  }
  return absl::bit_cast<const T*>(opaque);
}

// getrf: LU decomposition

struct GetrfDescriptor {
  Type type;
  int batch, m, n;
};

// Returns the workspace size and a descriptor for a getrf operation.
std::pair<int, py::bytes> BuildGetrfDescriptor(const py::dtype& dtype, int b,
                                               int m, int n) {
  Type type = DtypeToType(dtype);
  auto handle = SolverHandlePool::Borrow();
  int lwork;
  switch (type) {
    case Type::F32:
      ThrowIfErrorStatus(cusolverDnSgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork));
      break;
    case Type::F64:
      ThrowIfErrorStatus(cusolverDnDgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork));
      break;
    case Type::C64:
      ThrowIfErrorStatus(cusolverDnCgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork));
      break;
    case Type::C128:
      ThrowIfErrorStatus(cusolverDnZgetrf_bufferSize(handle.get(), m, n,
                                                     /*A=*/nullptr,
                                                     /*lda=*/m, &lwork));
      break;
  }
  return {lwork, PackDescriptor(GetrfDescriptor{type, b, m, n})};
}

void Getrf(cudaStream_t stream, void** buffers, const char* opaque,
           size_t opaque_len) {
  const GetrfDescriptor& d =
      *UnpackDescriptor<GetrfDescriptor>(opaque, opaque_len);
  auto handle = SolverHandlePool::Borrow(stream);
  ThrowIfError(cudaMemcpyAsync(buffers[1], buffers[0],
                               SizeOfType(d.type) * d.batch * d.m * d.n,
                               cudaMemcpyDeviceToDevice, stream));

  void* workspace = buffers[2];
  int* ipiv = static_cast<int*>(buffers[3]);
  int* info = static_cast<int*>(buffers[4]);
  switch (d.type) {
    case Type::F32: {
      float* a = static_cast<float*>(buffers[1]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnSgetrf(handle.get(), d.m, d.n, a, d.m,
                                            static_cast<float*>(workspace),
                                            ipiv, info));
        a += d.m * d.n;
        ipiv += std::min(d.m, d.n);
        ++info;
      }
      break;
    }
    case Type::F64: {
      double* a = static_cast<double*>(buffers[1]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnDgetrf(handle.get(), d.m, d.n, a, d.m,
                                            static_cast<double*>(workspace),
                                            ipiv, info));
        a += d.m * d.n;
        ipiv += std::min(d.m, d.n);
        ++info;
      }
      break;
    }
    case Type::C64: {
      cuComplex* a = static_cast<cuComplex*>(buffers[1]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnCgetrf(handle.get(), d.m, d.n, a, d.m,
                                            static_cast<cuComplex*>(workspace),
                                            ipiv, info));
        a += d.m * d.n;
        ipiv += std::min(d.m, d.n);
        ++info;
      }
      break;
    }
    case Type::C128: {
      cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnZgetrf(
            handle.get(), d.m, d.n, a, d.m,
            static_cast<cuDoubleComplex*>(workspace), ipiv, info));
        a += d.m * d.n;
        ipiv += std::min(d.m, d.n);
        ++info;
      }
      break;
    }
  }
}

// Symmetric (Hermitian) eigendecomposition, QR algorithm: syevd/heevd

struct SyevdDescriptor {
  Type type;
  cublasFillMode_t uplo;
  int batch, n;
  int lwork;
};

// Returns the workspace size and a descriptor for a syevd operation.
std::pair<int, py::bytes> BuildSyevdDescriptor(const py::dtype& dtype,
                                               bool lower, int b, int n) {
  Type type = DtypeToType(dtype);
  auto handle = SolverHandlePool::Borrow();
  int lwork;
  cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR;
  cublasFillMode_t uplo =
      lower ? CUBLAS_FILL_MODE_LOWER : CUBLAS_FILL_MODE_UPPER;
  switch (type) {
    case Type::F32:
      ThrowIfErrorStatus(cusolverDnSsyevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork));
      break;
    case Type::F64:
      ThrowIfErrorStatus(cusolverDnDsyevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork));
      break;
    case Type::C64:
      ThrowIfErrorStatus(cusolverDnCheevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork));
      break;
    case Type::C128:
      ThrowIfErrorStatus(cusolverDnZheevd_bufferSize(
          handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n, /*W=*/nullptr,
          &lwork));
      break;
  }
  return {lwork, PackDescriptor(SyevdDescriptor{type, uplo, b, n, lwork})};
}

void Syevd(cudaStream_t stream, void** buffers, const char* opaque,
           size_t opaque_len) {
  const SyevdDescriptor& d =
      *UnpackDescriptor<SyevdDescriptor>(opaque, opaque_len);
  auto handle = SolverHandlePool::Borrow(stream);
  ThrowIfError(cudaMemcpyAsync(buffers[1], buffers[0],
                               SizeOfType(d.type) * d.batch * d.n * d.n,
                               cudaMemcpyDeviceToDevice, stream));
  cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR;
  int* info = static_cast<int*>(buffers[3]);
  void* work = buffers[4];
  switch (d.type) {
    case Type::F32: {
      float* a = static_cast<float*>(buffers[1]);
      float* w = static_cast<float*>(buffers[2]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnSsyevd(handle.get(), jobz, d.uplo, d.n, a,
                                            d.n, w, static_cast<float*>(work),
                                            d.lwork, info));
        a += d.n * d.n;
        w += d.n;
        ++info;
      }
      break;
    }
    case Type::F64: {
      double* a = static_cast<double*>(buffers[1]);
      double* w = static_cast<double*>(buffers[2]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnDsyevd(handle.get(), jobz, d.uplo, d.n, a,
                                            d.n, w, static_cast<double*>(work),
                                            d.lwork, info));
        a += d.n * d.n;
        w += d.n;
        ++info;
      }
      break;
    }
    case Type::C64: {
      cuComplex* a = static_cast<cuComplex*>(buffers[1]);
      float* w = static_cast<float*>(buffers[2]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(
            cusolverDnCheevd(handle.get(), jobz, d.uplo, d.n, a, d.n, w,
                             static_cast<cuComplex*>(work), d.lwork, info));
        a += d.n * d.n;
        w += d.n;
        ++info;
      }
      break;
    }
    case Type::C128: {
      cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
      double* w = static_cast<double*>(buffers[2]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnZheevd(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<cuDoubleComplex*>(work), d.lwork, info));
        a += d.n * d.n;
        w += d.n;
        ++info;
      }
      break;
    }
  }
}

// Symmetric (Hermitian) eigendecomposition, Jacobi algorithm: syevj/heevj
// Supports batches of matrices up to size 32.

struct SyevjDescriptor {
  Type type;
  cublasFillMode_t uplo;
  int batch, n;
  int lwork;
};

// Returns the workspace size and a descriptor for a syevj_batched operation.
std::pair<int, py::bytes> BuildSyevjDescriptor(const py::dtype& dtype,
                                               bool lower, int batch, int n) {
  Type type = DtypeToType(dtype);
  auto handle = SolverHandlePool::Borrow();
  int lwork;
  syevjInfo_t params;
  ThrowIfErrorStatus(cusolverDnCreateSyevjInfo(&params));
  std::unique_ptr<syevjInfo, void (*)(syevjInfo*)> params_cleanup(
      params, [](syevjInfo* p) { cusolverDnDestroySyevjInfo(p); });
  cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR;
  cublasFillMode_t uplo =
      lower ? CUBLAS_FILL_MODE_LOWER : CUBLAS_FILL_MODE_UPPER;
  if (batch == 1) {
    switch (type) {
      case Type::F32:
        ThrowIfErrorStatus(cusolverDnSsyevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params));
        break;
      case Type::F64:
        ThrowIfErrorStatus(cusolverDnDsyevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params));
        break;
      case Type::C64:
        ThrowIfErrorStatus(cusolverDnCheevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params));
        break;
      case Type::C128:
        ThrowIfErrorStatus(cusolverDnZheevj_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params));
        break;
    }
  } else {
    switch (type) {
      case Type::F32:
        ThrowIfErrorStatus(cusolverDnSsyevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch));
        break;
      case Type::F64:
        ThrowIfErrorStatus(cusolverDnDsyevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch));
        break;
      case Type::C64:
        ThrowIfErrorStatus(cusolverDnCheevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch));
        break;
      case Type::C128:
        ThrowIfErrorStatus(cusolverDnZheevjBatched_bufferSize(
            handle.get(), jobz, uplo, n, /*A=*/nullptr, /*lda=*/n,
            /*W=*/nullptr, &lwork, params, batch));
        break;
    }
  }
  return {lwork, PackDescriptor(SyevjDescriptor{type, uplo, batch, n, lwork})};
}

void Syevj(cudaStream_t stream, void** buffers, const char* opaque,
           size_t opaque_len) {
  const SyevjDescriptor& d =
      *UnpackDescriptor<SyevjDescriptor>(opaque, opaque_len);
  auto handle = SolverHandlePool::Borrow(stream);
  if (buffers[1] != buffers[0]) {
    ThrowIfError(cudaMemcpyAsync(buffers[1], buffers[0],
                                 SizeOfType(d.type) * d.batch * d.n * d.n,
                                 cudaMemcpyDeviceToDevice, stream));
  }
  syevjInfo_t params;
  ThrowIfErrorStatus(cusolverDnCreateSyevjInfo(&params));
  std::unique_ptr<syevjInfo, void (*)(syevjInfo*)> params_cleanup(
      params, [](syevjInfo* p) { cusolverDnDestroySyevjInfo(p); });

  cusolverEigMode_t jobz = CUSOLVER_EIG_MODE_VECTOR;
  int* info = static_cast<int*>(buffers[3]);
  void* work = buffers[4];
  if (d.batch == 1) {
    switch (d.type) {
      case Type::F32: {
        float* a = static_cast<float*>(buffers[1]);
        float* w = static_cast<float*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnSsyevj(handle.get(), jobz, d.uplo, d.n, a,
                                            d.n, w, static_cast<float*>(work),
                                            d.lwork, info, params));
        break;
      }
      case Type::F64: {
        double* a = static_cast<double*>(buffers[1]);
        double* w = static_cast<double*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnDsyevj(handle.get(), jobz, d.uplo, d.n, a,
                                            d.n, w, static_cast<double*>(work),
                                            d.lwork, info, params));
        break;
      }
      case Type::C64: {
        cuComplex* a = static_cast<cuComplex*>(buffers[1]);
        float* w = static_cast<float*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnCheevj(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<cuComplex*>(work), d.lwork, info, params));
        break;
      }
      case Type::C128: {
        cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
        double* w = static_cast<double*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnZheevj(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<cuDoubleComplex*>(work), d.lwork, info, params));
        break;
      }
    }
  } else {
    switch (d.type) {
      case Type::F32: {
        float* a = static_cast<float*>(buffers[1]);
        float* w = static_cast<float*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnSsyevjBatched(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<float*>(work), d.lwork, info, params, d.batch));
        break;
      }
      case Type::F64: {
        double* a = static_cast<double*>(buffers[1]);
        double* w = static_cast<double*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnDsyevjBatched(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<double*>(work), d.lwork, info, params, d.batch));
        break;
      }
      case Type::C64: {
        cuComplex* a = static_cast<cuComplex*>(buffers[1]);
        float* w = static_cast<float*>(buffers[2]);
        ThrowIfErrorStatus(cusolverDnCheevjBatched(
            handle.get(), jobz, d.uplo, d.n, a, d.n, w,
            static_cast<cuComplex*>(work), d.lwork, info, params, d.batch));
        break;
      }
      case Type::C128: {
        cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
        double* w = static_cast<double*>(buffers[2]);
        ThrowIfErrorStatus(
            cusolverDnZheevjBatched(handle.get(), jobz, d.uplo, d.n, a, d.n, w,
                                    static_cast<cuDoubleComplex*>(work),
                                    d.lwork, info, params, d.batch));
        break;
      }
    }
  }
}

// Singular value decomposition: gesvd

struct GesvdDescriptor {
  Type type;
  int batch, m, n;
  int lwork;
  signed char jobu, jobvt;
};

// Returns the workspace size and a descriptor for a gesvd operation.
std::pair<int, py::bytes> BuildGesvdDescriptor(const py::dtype& dtype, int b,
                                               int m, int n, bool compute_uv,
                                               bool full_matrices) {
  Type type = DtypeToType(dtype);
  auto handle = SolverHandlePool::Borrow();
  int lwork;
  switch (type) {
    case Type::F32:
      ThrowIfErrorStatus(
          cusolverDnSgesvd_bufferSize(handle.get(), m, n, &lwork));
      break;
    case Type::F64:
      ThrowIfErrorStatus(
          cusolverDnDgesvd_bufferSize(handle.get(), m, n, &lwork));
      break;
    case Type::C64:
      ThrowIfErrorStatus(
          cusolverDnCgesvd_bufferSize(handle.get(), m, n, &lwork));
      break;
    case Type::C128:
      ThrowIfErrorStatus(
          cusolverDnZgesvd_bufferSize(handle.get(), m, n, &lwork));
      break;
  }
  signed char jobu, jobvt;
  if (compute_uv) {
    if (full_matrices) {
      jobu = jobvt = 'A';
    } else {
      jobu = jobvt = 'S';
    }
  } else {
    jobu = jobvt = 'N';
  }
  return {lwork,
          PackDescriptor(GesvdDescriptor{type, b, m, n, lwork, jobu, jobvt})};
}

// TODO(phawkins): in the batched case, we should consider using the batched
// Jacobi implementation instead.
void Gesvd(cudaStream_t stream, void** buffers, const char* opaque,
           size_t opaque_len) {
  const GesvdDescriptor& d =
      *UnpackDescriptor<GesvdDescriptor>(opaque, opaque_len);
  auto handle = SolverHandlePool::Borrow(stream);
  ThrowIfError(cudaMemcpyAsync(buffers[1], buffers[0],
                               SizeOfType(d.type) * d.batch * d.m * d.n,
                               cudaMemcpyDeviceToDevice, stream));
  int* info = static_cast<int*>(buffers[5]);
  void* work = buffers[6];
  switch (d.type) {
    case Type::F32: {
      float* a = static_cast<float*>(buffers[1]);
      float* s = static_cast<float*>(buffers[2]);
      float* u = static_cast<float*>(buffers[3]);
      float* vt = static_cast<float*>(buffers[4]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnSgesvd(handle.get(), d.jobu, d.jobvt, d.m,
                                            d.n, a, d.m, s, u, d.m, vt, d.n,
                                            static_cast<float*>(work), d.lwork,
                                            /*rwork=*/nullptr, info));
        a += d.m * d.n;
        s += std::min(d.m, d.n);
        u += d.m * d.m;
        vt += d.n * d.n;
        ++info;
      }
      break;
    }
    case Type::F64: {
      double* a = static_cast<double*>(buffers[1]);
      double* s = static_cast<double*>(buffers[2]);
      double* u = static_cast<double*>(buffers[3]);
      double* vt = static_cast<double*>(buffers[4]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnDgesvd(handle.get(), d.jobu, d.jobvt, d.m,
                                            d.n, a, d.m, s, u, d.m, vt, d.n,
                                            static_cast<double*>(work), d.lwork,
                                            /*rwork=*/nullptr, info));
        a += d.m * d.n;
        s += std::min(d.m, d.n);
        u += d.m * d.m;
        vt += d.n * d.n;
        ++info;
      }
      break;
    }
    case Type::C64: {
      cuComplex* a = static_cast<cuComplex*>(buffers[1]);
      float* s = static_cast<float*>(buffers[2]);
      cuComplex* u = static_cast<cuComplex*>(buffers[3]);
      cuComplex* vt = static_cast<cuComplex*>(buffers[4]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnCgesvd(
            handle.get(), d.jobu, d.jobvt, d.m, d.n, a, d.m, s, u, d.m, vt, d.n,
            static_cast<cuComplex*>(work), d.lwork, /*rwork=*/nullptr, info));
        a += d.m * d.n;
        s += std::min(d.m, d.n);
        u += d.m * d.m;
        vt += d.n * d.n;
        ++info;
      }
      break;
    }
    case Type::C128: {
      cuDoubleComplex* a = static_cast<cuDoubleComplex*>(buffers[1]);
      double* s = static_cast<double*>(buffers[2]);
      cuDoubleComplex* u = static_cast<cuDoubleComplex*>(buffers[3]);
      cuDoubleComplex* vt = static_cast<cuDoubleComplex*>(buffers[4]);
      for (int i = 0; i < d.batch; ++i) {
        ThrowIfErrorStatus(cusolverDnZgesvd(
            handle.get(), d.jobu, d.jobvt, d.m, d.n, a, d.m, s, u, d.m, vt, d.n,
            static_cast<cuDoubleComplex*>(work), d.lwork,
            /*rwork=*/nullptr, info));
        a += d.m * d.n;
        s += std::min(d.m, d.n);
        u += d.m * d.m;
        vt += d.n * d.n;
        ++info;
      }
      break;
    }
  }
}

template <typename T>
py::capsule EncapsulateFunction(T* fn) {
  return py::capsule(absl::bit_cast<void*>(fn), "xla._CUSTOM_CALL_TARGET");
}

py::dict Registrations() {
  py::dict dict;
  dict["cusolver_getrf"] = EncapsulateFunction(Getrf);
  dict["cusolver_syevd"] = EncapsulateFunction(Syevd);
  dict["cusolver_syevj"] = EncapsulateFunction(Syevj);
  dict["cusolver_gesvd"] = EncapsulateFunction(Gesvd);
  return dict;
}

PYBIND11_MODULE(cusolver_kernels, m) {
  m.def("registrations", &Registrations);
  m.def("build_getrf_descriptor", &BuildGetrfDescriptor);
  m.def("build_syevd_descriptor", &BuildSyevdDescriptor);
  m.def("build_syevj_descriptor", &BuildSyevjDescriptor);
  m.def("build_gesvd_descriptor", &BuildGesvdDescriptor);
}

}  // namespace
}  // namespace jax
