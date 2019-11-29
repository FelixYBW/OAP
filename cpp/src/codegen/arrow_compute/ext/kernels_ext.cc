#include "codegen/arrow_compute/ext/kernels_ext.h"
#include <arrow/compute/context.h>
#include <arrow/compute/kernel.h>
#include <arrow/compute/kernels/count.h>
#include <arrow/compute/kernels/hash.h>
#include <arrow/compute/kernels/minmax.h>
#include <arrow/compute/kernels/sum.h>
#include <arrow/pretty_print.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/type_traits.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/checked_cast.h>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include "codegen/arrow_compute/ext/actions_impl.h"
#include "codegen/arrow_compute/ext/append.h"
#include "codegen/arrow_compute/ext/split.h"

namespace sparkcolumnarplugin {
namespace codegen {
namespace arrowcompute {
namespace extra {

///////////////  SplitArrayList  ////////////////
arrow::Status SplitArrayList(arrow::compute::FunctionContext* ctx, const ArrayList& in,
                             const std::shared_ptr<arrow::Array>& in_dict,
                             std::vector<ArrayList>* out, std::vector<int>* out_sizes,
                             std::vector<int>* group_indices) {
  if (!in_dict) {
    return arrow::Status::Invalid("input data is invalid");
  }
  std::vector<std::shared_ptr<splitter::ArrayVisitorImpl>> array_visitor_list;
  std::vector<std::shared_ptr<ArrayBuilderImplBase>> builder_list_;
  std::unordered_map<int, int> group_id_to_index;
  for (auto col : in) {
    auto visitor = std::make_shared<splitter::ArrayVisitorImpl>(ctx);
    RETURN_NOT_OK(col->Accept(&(*visitor.get())));
    array_visitor_list.push_back(visitor);

    std::shared_ptr<ArrayBuilderImplBase> builder;
    visitor->GetBuilder(&builder);
    builder_list_.push_back(builder);
  }

  auto dict_array = std::dynamic_pointer_cast<arrow::DictionaryArray>(in_dict);
  auto dict = dict_array->indices();
  auto values = dict_array->dictionary();

  for (int row_id = 0; row_id < dict->length(); row_id++) {
    auto group_id = arrow::internal::checked_cast<const arrow::Int32Array&>(*dict.get())
                        .GetView(row_id);
    auto find = group_id_to_index.find(group_id);
    int index;
    if (find == group_id_to_index.end()) {
      index = group_id_to_index.size();
      group_id_to_index.emplace(group_id, index);
      group_indices->push_back(group_id);
    } else {
      index = find->second;
    }
    for (int i = 0; i < array_visitor_list.size(); i++) {
      array_visitor_list[i]->Eval(builder_list_[i], index, row_id);
    }
  }

  for (auto builder : builder_list_) {
    std::vector<std::shared_ptr<arrow::Array>> arr_list_out;
    RETURN_NOT_OK(builder->Finish(&arr_list_out));
    for (int i = 0; i < arr_list_out.size(); i++) {
      if (out->size() <= i) {
        ArrayList arr_list;
        out->push_back(arr_list);
        out_sizes->push_back(arr_list_out[i]->length());
      }
      out->at(i).push_back(arr_list_out[i]);
    }
  }

  return arrow::Status::OK();
}

///////////////  SplitArrayListWithAction  ////////////////
class SplitArrayListWithActionKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx, std::vector<std::string> action_name_list)
      : ctx_(ctx), action_name_list_(action_name_list) {}
  ~Impl() {
#ifdef DEBUG
    std::cout << "Destruct SplitArrayListWithActionKernel::Impl" << std::endl;
#endif
  }

  arrow::Status InitActionList(const ArrayList& in) {
    int col_id = 0;
    for (auto action_name : action_name_list_) {
      auto col = in[col_id++];
      std::shared_ptr<ActionBase> action;
      if (action_name.compare("action_unique") == 0) {
        RETURN_NOT_OK(MakeUniqueAction(ctx_, col->type(), &action));
      } else if (action_name.compare("action_count") == 0) {
        RETURN_NOT_OK(MakeCountAction(ctx_, &action));
      } else if (action_name.compare("action_sum") == 0) {
        RETURN_NOT_OK(MakeSumAction(ctx_, col->type(), &action));
      } else {
        return arrow::Status::NotImplemented(action_name, " is not implementetd.");
      }
      action_list_.push_back(action);
    }
    return arrow::Status::OK();
  }

  arrow::Status Evaluate(const ArrayList& in,
                         const std::shared_ptr<arrow::Array>& in_dict) {
    if (!in_dict) {
      return arrow::Status::Invalid("input data is invalid");
    }
    auto dict_array = std::dynamic_pointer_cast<arrow::DictionaryArray>(in_dict);
    auto dict_generic = dict_array->indices();
    auto dict = std::dynamic_pointer_cast<arrow::Int32Array>(dict_generic);
    auto values = dict_array->dictionary();

    if (action_list_.empty()) {
      RETURN_NOT_OK(InitActionList(in));
    }

    std::vector<std::shared_ptr<splitter::ArrayVisitorImpl>> array_visitor_list;
    if (in.size() != action_list_.size()) {
      return arrow::Status::Invalid(
          "SplitArrayListWithAction input arrayList size does not match numActions");
    }

    // using minmax
    arrow::compute::MinMaxOptions options;
    arrow::compute::Datum minMaxOut;
    RETURN_NOT_OK(arrow::compute::MinMax(ctx_, options, *dict.get(), &minMaxOut));
    auto col = minMaxOut.collection();
    auto max =
        arrow::internal::checked_pointer_cast<arrow::UInt32Scalar>(col[1].scalar());
    auto max_group_id = max->value;

    std::vector<std::function<arrow::Status(int)>> eval_func_list;
    for (int i = 0; i < in.size(); i++) {
      auto col = in[i];
      auto action = action_list_[i];
      std::function<arrow::Status(int)> func;
      action->Submit(col, max_group_id, &func);
      eval_func_list.push_back(func);
    }

    auto start = std::chrono::steady_clock::now();
    const uint32_t* data = dict->data()->GetValues<uint32_t>(1);
    for (int row_id = 0; row_id < dict->length(); row_id++) {
      auto group_id = data[row_id];
      for (auto eval_func : eval_func_list) {
        eval_func(group_id);
      }
    }
    auto end = std::chrono::steady_clock::now();
    eval_elapse_time_ +=
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    return arrow::Status::OK();
  }

  arrow::Status Finish(ArrayList* out) {
    for (auto action : action_list_) {
      std::shared_ptr<arrow::Array> arr_out;
      RETURN_NOT_OK(action->Finish(&arr_out));
      out->push_back(arr_out);
    }
    std::cout << "SplitArrayListWithActionKernel took " << eval_elapse_time_ / 1000
              << "ms doing evaluation." << std::endl;
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::vector<std::string> action_name_list_;
  std::vector<std::shared_ptr<extra::ActionBase>> action_list_;
};

arrow::Status SplitArrayListWithActionKernel::Make(
    arrow::compute::FunctionContext* ctx, std::vector<std::string> action_name_list,
    std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<SplitArrayListWithActionKernel>(ctx, action_name_list);
  return arrow::Status::OK();
}

SplitArrayListWithActionKernel::SplitArrayListWithActionKernel(
    arrow::compute::FunctionContext* ctx, std::vector<std::string> action_name_list) {
  impl_.reset(new Impl(ctx, action_name_list));
}

arrow::Status SplitArrayListWithActionKernel::Evaluate(
    const ArrayList& in, const std::shared_ptr<arrow::Array>& dict) {
  return impl_->Evaluate(in, dict);
}

arrow::Status SplitArrayListWithActionKernel::Finish(ArrayList* out) {
  return impl_->Finish(out);
}

///////////////  UniqueArray  ////////////////
class UniqueArrayKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {}
  ~Impl() {}
  arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in) {
    std::shared_ptr<arrow::Array> out;
    if (in->length() == 0) {
      return arrow::Status::OK();
    }
    arrow::compute::Datum input_datum(in);
    RETURN_NOT_OK(arrow::compute::Unique(ctx_, input_datum, &out));
    if (!builder) {
      RETURN_NOT_OK(MakeArrayBuilder(out->type(), ctx_->memory_pool(), &builder));
    }

    RETURN_NOT_OK(builder->AppendArray(&(*out.get()), 0, 0));

    return arrow::Status::OK();
  }

  arrow::Status Finish(std::shared_ptr<arrow::Array>* out) {
    RETURN_NOT_OK(builder->Finish(out));
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<ArrayBuilderImplBase> builder;
};

arrow::Status UniqueArrayKernel::Make(arrow::compute::FunctionContext* ctx,
                                      std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<UniqueArrayKernel>(ctx);
  return arrow::Status::OK();
}

UniqueArrayKernel::UniqueArrayKernel(arrow::compute::FunctionContext* ctx) {
  impl_.reset(new Impl(ctx));
}

arrow::Status UniqueArrayKernel::Evaluate(const std::shared_ptr<arrow::Array>& in) {
  return impl_->Evaluate(in);
}

arrow::Status UniqueArrayKernel::Finish(std::shared_ptr<arrow::Array>* out) {
  return impl_->Finish(out);
}

///////////////  SumArray  ////////////////
class SumArrayKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {}
  ~Impl() {}
  arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in) {
    arrow::compute::Datum output;
    // std::cout << "SumArray Evaluate Input is " << std::endl;
    // arrow::PrettyPrint(*in.get(), 2, &std::cout);
    RETURN_NOT_OK(arrow::compute::Sum(ctx_, *in.get(), &output));
    std::shared_ptr<arrow::Array> out;
    RETURN_NOT_OK(
        arrow::MakeArrayFromScalar(*(output.scalar()).get(), output.length(), &out));
    if (!builder) {
      RETURN_NOT_OK(MakeArrayBuilder(out->type(), ctx_->memory_pool(), &builder));
    }
    // std::cout << "SumArray Evaluate Output is " << std::endl;
    // arrow::PrettyPrint(*out.get(), 2, &std::cout);
    RETURN_NOT_OK(builder->AppendArray(&(*out.get()), 0, 0));
    // TODO: We should only append Scalar instead of array
    // RETURN_NOT_OK(builder->AppendScalar(output.scalar()));

    return arrow::Status::OK();
  }

  arrow::Status Finish(std::shared_ptr<arrow::Array>* out) {
    RETURN_NOT_OK(builder->Finish(out));
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<ArrayBuilderImplBase> builder;
};

arrow::Status SumArrayKernel::Make(arrow::compute::FunctionContext* ctx,
                                   std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<SumArrayKernel>(ctx);
  return arrow::Status::OK();
}

SumArrayKernel::SumArrayKernel(arrow::compute::FunctionContext* ctx) {
  impl_.reset(new Impl(ctx));
}

arrow::Status SumArrayKernel::Evaluate(const std::shared_ptr<arrow::Array>& in) {
  return impl_->Evaluate(in);
}

arrow::Status SumArrayKernel::Finish(std::shared_ptr<arrow::Array>* out) {
  return impl_->Finish(out);
}

///////////////  CountArray  ////////////////
class CountArrayKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {}
  ~Impl() {}
  arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in) {
    arrow::compute::Datum output;
    arrow::compute::CountOptions opt =
        arrow::compute::CountOptions(arrow::compute::CountOptions::COUNT_ALL);
    RETURN_NOT_OK(arrow::compute::Count(ctx_, opt, *in.get(), &output));
    std::shared_ptr<arrow::Array> out;
    RETURN_NOT_OK(
        arrow::MakeArrayFromScalar(*(output.scalar()).get(), output.length(), &out));
    if (!builder) {
      RETURN_NOT_OK(MakeArrayBuilder(out->type(), ctx_->memory_pool(), &builder));
    }

    RETURN_NOT_OK(builder->AppendArray(&(*out.get()), 0, 0));
    // TODO: We should only append Scalar instead of array

    return arrow::Status::OK();
  }

  arrow::Status Finish(std::shared_ptr<arrow::Array>* out) {
    RETURN_NOT_OK(builder->Finish(out));
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<ArrayBuilderImplBase> builder;
};

arrow::Status CountArrayKernel::Make(arrow::compute::FunctionContext* ctx,
                                     std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<CountArrayKernel>(ctx);
  return arrow::Status::OK();
}

CountArrayKernel::CountArrayKernel(arrow::compute::FunctionContext* ctx) {
  impl_.reset(new Impl(ctx));
}

arrow::Status CountArrayKernel::Evaluate(const std::shared_ptr<arrow::Array>& in) {
  return impl_->Evaluate(in);
}

arrow::Status CountArrayKernel::Finish(std::shared_ptr<arrow::Array>* out) {
  return impl_->Finish(out);
}

///////////////  EncodeArray  ////////////////
class EncodeArrayKernel::Impl {
 public:
  Impl() {}
  ~Impl() {}
  virtual arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in,
                                 std::shared_ptr<arrow::Array>* out) = 0;
};

template <typename InType, typename MemoTableType>
class EncodeArrayTypedImpl : public EncodeArrayKernel::Impl {
 public:
  EncodeArrayTypedImpl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {
    hash_table_ = std::make_shared<MemoTableType>(ctx_->memory_pool());
  }
  ~EncodeArrayTypedImpl() {}
  arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in,
                         std::shared_ptr<arrow::Array>* out) {
    arrow::compute::Datum input_datum(in);

    arrow::compute::Datum out_dict;
    RETURN_NOT_OK(arrow::compute::DictionaryEncode<InType>(ctx_, input_datum, hash_table_,
                                                           &out_dict));
    *out = out_dict.make_array();
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<MemoTableType> hash_table_;
};

arrow::Status EncodeArrayKernel::Make(arrow::compute::FunctionContext* ctx,
                                      std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<EncodeArrayKernel>(ctx);
  return arrow::Status::OK();
}

EncodeArrayKernel::EncodeArrayKernel(arrow::compute::FunctionContext* ctx) { ctx_ = ctx; }

#define PROCESS_SUPPORTED_TYPES(PROCESS) \
  PROCESS(arrow::BooleanType)            \
  PROCESS(arrow::UInt8Type)              \
  PROCESS(arrow::Int8Type)               \
  PROCESS(arrow::UInt16Type)             \
  PROCESS(arrow::Int16Type)              \
  PROCESS(arrow::UInt32Type)             \
  PROCESS(arrow::Int32Type)              \
  PROCESS(arrow::UInt64Type)             \
  PROCESS(arrow::Int64Type)              \
  PROCESS(arrow::FloatType)              \
  PROCESS(arrow::DoubleType)             \
  PROCESS(arrow::Date32Type)             \
  PROCESS(arrow::Date64Type)             \
  PROCESS(arrow::Time32Type)             \
  PROCESS(arrow::Time64Type)             \
  PROCESS(arrow::TimestampType)          \
  PROCESS(arrow::BinaryType)             \
  PROCESS(arrow::StringType)             \
  PROCESS(arrow::FixedSizeBinaryType)    \
  PROCESS(arrow::Decimal128Type)
arrow::Status EncodeArrayKernel::Evaluate(const std::shared_ptr<arrow::Array>& in,
                                          std::shared_ptr<arrow::Array>* out) {
  if (!impl_) {
    switch (in->type_id()) {
#define PROCESS(InType)                                                                \
  case InType::type_id: {                                                              \
    using MemoTableType = typename arrow::internal::HashTraits<InType>::MemoTableType; \
    impl_.reset(new EncodeArrayTypedImpl<InType, MemoTableType>(ctx_));                \
  } break;
      PROCESS_SUPPORTED_TYPES(PROCESS)
#undef PROCESS
      default:
        break;
    }
  }
  return impl_->Evaluate(in, out);
}
#undef PROCESS_SUPPORTED_TYPES

///////////////  AppendToCacheArrayList  ////////////////
class AppendToCacheArrayListKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {}
  ~Impl() {}
  arrow::Status Evaluate(const ArrayList& in) {
    std::vector<std::shared_ptr<appender::ArrayVisitorImpl>> array_visitor_list;
    for (auto col : in) {
      auto visitor = std::make_shared<appender::ArrayVisitorImpl>(ctx_);
      RETURN_NOT_OK(col->Accept(&(*visitor.get())));
      array_visitor_list.push_back(visitor);
    }

    int need_to_append = array_visitor_list.size() - builder_list_.size() + 1;
    if (need_to_append < 0) {
      return arrow::Status::Invalid(
          "AppendToCacheArrayListKernel::Impl array size is smaller than total array "
          "builder size, unable to map the relation.appender");
    }

    for (int i = 0; i < array_visitor_list.size(); i++) {
      std::shared_ptr<ArrayBuilderImplBase> builder;
      if (builder_list_.size() <= i) {
        RETURN_NOT_OK(array_visitor_list[i]->GetBuilder(&builder));
        builder_list_.push_back(builder);

      } else {
        builder = builder_list_[i];
      }
      RETURN_NOT_OK(array_visitor_list[i]->Eval(builder));
    }

    return arrow::Status::OK();
  }

  arrow::Status Finish(ArrayList* out) {
    for (auto builder : builder_list_) {
      std::shared_ptr<arrow::Array> arr_out;
      RETURN_NOT_OK(builder->Finish(&arr_out));
      out->push_back(arr_out);
    }
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::vector<std::shared_ptr<ArrayBuilderImplBase>> builder_list_;
};
arrow::Status AppendToCacheArrayListKernel::Make(arrow::compute::FunctionContext* ctx,
                                                 std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<AppendToCacheArrayListKernel>(ctx);
  return arrow::Status::OK();
}

AppendToCacheArrayListKernel::AppendToCacheArrayListKernel(
    arrow::compute::FunctionContext* ctx) {
  impl_.reset(new Impl(ctx));
}

arrow::Status AppendToCacheArrayListKernel::Evaluate(const ArrayList& in) {
  return impl_->Evaluate(in);
}

arrow::Status AppendToCacheArrayListKernel::Finish(ArrayList* out) {
  return impl_->Finish(out);
}

///////////////  AppendToCacheArray  ////////////////
class AppendToCacheArrayKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx) : ctx_(ctx) {}
  ~Impl() {}
  arrow::Status Evaluate(const std::shared_ptr<arrow::Array>& in, int group_id) {
    auto visitor = std::make_shared<appender::ArrayVisitorImpl>(ctx_);
    RETURN_NOT_OK(in->Accept(&(*visitor.get())));

    if (!builder) {
      RETURN_NOT_OK(visitor->GetBuilder(&builder));
    }
    RETURN_NOT_OK(visitor->Eval(builder, group_id));

    return arrow::Status::OK();
  }

  arrow::Status Finish(ArrayList* out) {
    RETURN_NOT_OK(builder->Finish(out));
    return arrow::Status::OK();
  }

  arrow::Status Finish(std::shared_ptr<arrow::Array>* out) {
    RETURN_NOT_OK(builder->Finish(out));
    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  std::shared_ptr<ArrayBuilderImplBase> builder;
};
arrow::Status AppendToCacheArrayKernel::Make(arrow::compute::FunctionContext* ctx,
                                             std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<AppendToCacheArrayKernel>(ctx);
  return arrow::Status::OK();
}

AppendToCacheArrayKernel::AppendToCacheArrayKernel(arrow::compute::FunctionContext* ctx) {
  impl_.reset(new Impl(ctx));
}

arrow::Status AppendToCacheArrayKernel::Evaluate(const std::shared_ptr<arrow::Array>& in,
                                                 int group_id) {
  return impl_->Evaluate(in, group_id);
}

arrow::Status AppendToCacheArrayKernel::Finish(std::shared_ptr<arrow::Array>* out) {
  return impl_->Finish(out);
}

arrow::Status AppendToCacheArrayKernel::Finish(ArrayList* out) {
  return impl_->Finish(out);
}

}  // namespace extra
}  // namespace arrowcompute
}  // namespace codegen
}  // namespace sparkcolumnarplugin
