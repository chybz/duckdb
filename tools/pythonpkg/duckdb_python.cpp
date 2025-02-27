#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>

#include <unordered_map>
#include <vector>
#include <atomic>

#include "datetime.h" // from Python

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/arrow.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "extension/extension_helper.hpp"
#include "duckdb/parallel/parallel_state.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/printer.hpp"
#include <random>
#include <stdlib.h>

namespace py = pybind11;

namespace duckdb {
namespace duckdb_py_convert {

struct RegularConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static NUMPY_T ConvertValue(DUCKDB_T val) {
		return (NUMPY_T)val;
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return 0;
	}
};

struct TimestampConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static int64_t ConvertValue(timestamp_t val) {
		return Timestamp::GetEpochNanoSeconds(val);
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return 0;
	}
};

struct DateConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static int64_t ConvertValue(date_t val) {
		return Date::EpochNanoseconds(val);
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return 0;
	}
};

struct TimeConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static PyObject *ConvertValue(dtime_t val) {
		auto str = duckdb::Time::ToString(val);
		return PyUnicode_FromStringAndSize(str.c_str(), str.size());
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return nullptr;
	}
};

struct StringConvert {
#if PY_MAJOR_VERSION >= 3
	template <class T>
	static void ConvertUnicodeValueTemplated(T *result, int32_t *codepoints, idx_t codepoint_count, const char *data,
	                                         idx_t ascii_count) {
		// we first fill in the batch of ascii characters directly
		for (idx_t i = 0; i < ascii_count; i++) {
			result[i] = data[i];
		}
		// then we fill in the remaining codepoints from our codepoint array
		for (idx_t i = 0; i < codepoint_count; i++) {
			result[ascii_count + i] = codepoints[i];
		}
	}

	static PyObject *ConvertUnicodeValue(const char *data, idx_t len, idx_t start_pos) {
		// slow path: check the code points
		// we know that all characters before "start_pos" were ascii characters, so we don't need to check those

		// allocate an array of code points so we only have to convert the codepoints once
		// short-string optimization
		// we know that the max amount of codepoints is the length of the string
		// for short strings (less than 64 bytes) we simply statically allocate an array of 256 bytes (64x int32)
		// this avoids memory allocation for small strings (common case)
		idx_t remaining = len - start_pos;
		unique_ptr<int32_t[]> allocated_codepoints;
		int32_t static_codepoints[64];
		int32_t *codepoints;
		if (remaining > 64) {
			allocated_codepoints = unique_ptr<int32_t[]>(new int32_t[remaining]);
			codepoints = allocated_codepoints.get();
		} else {
			codepoints = static_codepoints;
		}
		// now we iterate over the remainder of the string to convert the UTF8 string into a sequence of codepoints
		// and to find the maximum codepoint
		int32_t max_codepoint = 127;
		int sz;
		idx_t pos = start_pos;
		idx_t codepoint_count = 0;
		while (pos < len) {
			codepoints[codepoint_count] = Utf8Proc::UTF8ToCodepoint(data + pos, sz);
			pos += sz;
			if (codepoints[codepoint_count] > max_codepoint) {
				max_codepoint = codepoints[codepoint_count];
			}
			codepoint_count++;
		}
		// based on the max codepoint, we construct the result string
		auto result = PyUnicode_New(start_pos + codepoint_count, max_codepoint);
		// based on the resulting unicode kind, we fill in the code points
		auto kind = PyUnicode_KIND(result);
		switch (kind) {
		case PyUnicode_1BYTE_KIND:
			ConvertUnicodeValueTemplated<Py_UCS1>(PyUnicode_1BYTE_DATA(result), codepoints, codepoint_count, data,
			                                      start_pos);
			break;
		case PyUnicode_2BYTE_KIND:
			ConvertUnicodeValueTemplated<Py_UCS2>(PyUnicode_2BYTE_DATA(result), codepoints, codepoint_count, data,
			                                      start_pos);
			break;
		case PyUnicode_4BYTE_KIND:
			ConvertUnicodeValueTemplated<Py_UCS4>(PyUnicode_4BYTE_DATA(result), codepoints, codepoint_count, data,
			                                      start_pos);
			break;
		default:
			throw std::runtime_error("Unsupported typekind for Python Unicode Compact decode");
		}
		return result;
	}

	template <class DUCKDB_T, class NUMPY_T>
	static PyObject *ConvertValue(string_t val) {
		// we could use PyUnicode_FromStringAndSize here, but it does a lot of verification that we don't need
		// because of that it is a lot slower than it needs to be
		auto data = (uint8_t *)val.GetDataUnsafe();
		auto len = val.GetSize();
		// check if there are any non-ascii characters in there
		for (idx_t i = 0; i < len; i++) {
			if (data[i] > 127) {
				// there are! fallback to slower case
				return ConvertUnicodeValue((const char *)data, len, i);
			}
		}
		// no unicode: fast path
		// directly construct the string and memcpy it
		auto result = PyUnicode_New(len, 127);
		auto target_data = PyUnicode_DATA(result);
		memcpy(target_data, data, len);
		return result;
	}
#else
	template <class DUCKDB_T, class NUMPY_T>
	static PyObject *ConvertValue(string_t val) {
		return py::str(val.GetString()).release().ptr();
	}
#endif

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return nullptr;
	}
};

struct BlobConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static PyObject *ConvertValue(string_t val) {
		return PyByteArray_FromStringAndSize(val.GetDataUnsafe(), val.GetSize());
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return nullptr;
	}
};

struct IntegralConvert {
	template <class DUCKDB_T, class NUMPY_T>
	static NUMPY_T ConvertValue(DUCKDB_T val) {
		return NUMPY_T(val);
	}

	template <class NUMPY_T>
	static NUMPY_T NullValue() {
		return 0;
	}
};

template <>
double IntegralConvert::ConvertValue(hugeint_t val) {
	double result;
	Hugeint::TryCast(val, result);
	return result;
}

} // namespace duckdb_py_convert

template <class DUCKDB_T, class NUMPY_T, class CONVERT>
static bool ConvertColumn(idx_t target_offset, data_ptr_t target_data, bool *target_mask, VectorData &idata,
                          idx_t count) {
	auto src_ptr = (DUCKDB_T *)idata.data;
	auto out_ptr = (NUMPY_T *)target_data;
	if (!idata.validity.AllValid()) {
		for (idx_t i = 0; i < count; i++) {
			idx_t src_idx = idata.sel->get_index(i);
			idx_t offset = target_offset + i;
			if (!idata.validity.RowIsValidUnsafe(src_idx)) {
				target_mask[offset] = true;
				out_ptr[offset] = CONVERT::template NullValue<NUMPY_T>();
			} else {
				out_ptr[offset] = CONVERT::template ConvertValue<DUCKDB_T, NUMPY_T>(src_ptr[src_idx]);
				target_mask[offset] = false;
			}
		}
		return true;
	} else {
		for (idx_t i = 0; i < count; i++) {
			idx_t src_idx = idata.sel->get_index(i);
			idx_t offset = target_offset + i;
			out_ptr[offset] = CONVERT::template ConvertValue<DUCKDB_T, NUMPY_T>(src_ptr[src_idx]);
			target_mask[offset] = false;
		}
		return false;
	}
}

template <class T>
static bool ConvertColumnRegular(idx_t target_offset, data_ptr_t target_data, bool *target_mask, VectorData &idata,
                                 idx_t count) {
	return ConvertColumn<T, T, duckdb_py_convert::RegularConvert>(target_offset, target_data, target_mask, idata,
	                                                              count);
}

template <class DUCKDB_T>
static bool ConvertDecimalInternal(idx_t target_offset, data_ptr_t target_data, bool *target_mask, VectorData &idata,
                                   idx_t count, double division) {
	auto src_ptr = (DUCKDB_T *)idata.data;
	auto out_ptr = (double *)target_data;
	if (!idata.validity.AllValid()) {
		for (idx_t i = 0; i < count; i++) {
			idx_t src_idx = idata.sel->get_index(i);
			idx_t offset = target_offset + i;
			if (!idata.validity.RowIsValidUnsafe(src_idx)) {
				target_mask[offset] = true;
			} else {
				out_ptr[offset] =
				    duckdb_py_convert::IntegralConvert::ConvertValue<DUCKDB_T, double>(src_ptr[src_idx]) / division;
				target_mask[offset] = false;
			}
		}
		return true;
	} else {
		for (idx_t i = 0; i < count; i++) {
			idx_t src_idx = idata.sel->get_index(i);
			idx_t offset = target_offset + i;
			out_ptr[offset] =
			    duckdb_py_convert::IntegralConvert::ConvertValue<DUCKDB_T, double>(src_ptr[src_idx]) / division;
			target_mask[offset] = false;
		}
		return false;
	}
}

static bool ConvertDecimal(const LogicalType &decimal_type, idx_t target_offset, data_ptr_t target_data,
                           bool *target_mask, VectorData &idata, idx_t count) {
	auto dec_scale = decimal_type.scale();
	double division = pow(10, dec_scale);
	switch (decimal_type.InternalType()) {
	case PhysicalType::INT16:
		return ConvertDecimalInternal<int16_t>(target_offset, target_data, target_mask, idata, count, division);
	case PhysicalType::INT32:
		return ConvertDecimalInternal<int32_t>(target_offset, target_data, target_mask, idata, count, division);
	case PhysicalType::INT64:
		return ConvertDecimalInternal<int64_t>(target_offset, target_data, target_mask, idata, count, division);
	case PhysicalType::INT128:
		return ConvertDecimalInternal<hugeint_t>(target_offset, target_data, target_mask, idata, count, division);
	default:
		throw NotImplementedException("Unimplemented internal type for DECIMAL");
	}
}

struct RawArrayWrapper {
	explicit RawArrayWrapper(const LogicalType &type);

	py::array array;
	data_ptr_t data;
	LogicalType type;
	idx_t type_width;
	idx_t count;

public:
	void Initialize(idx_t capacity);
	void Resize(idx_t new_capacity);
	void Append(idx_t current_offset, Vector &input, idx_t count);
};

struct ArrayWrapper {
	explicit ArrayWrapper(const LogicalType &type);

	unique_ptr<RawArrayWrapper> data;
	unique_ptr<RawArrayWrapper> mask;
	bool requires_mask;

public:
	void Initialize(idx_t capacity);
	void Resize(idx_t new_capacity);
	void Append(idx_t current_offset, Vector &input, idx_t count);
	py::object ToArray(idx_t count) const;
};

class NumpyResultConversion {
public:
	NumpyResultConversion(vector<LogicalType> &types, idx_t initial_capacity);

	void Append(DataChunk &chunk);

	py::object ToArray(idx_t col_idx) {
		return owned_data[col_idx].ToArray(count);
	}

private:
	void Resize(idx_t new_capacity);

private:
	vector<ArrayWrapper> owned_data;
	idx_t count;
	idx_t capacity;
};

RawArrayWrapper::RawArrayWrapper(const LogicalType &type) : data(nullptr), type(type), count(0) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		type_width = sizeof(bool);
		break;
	case LogicalTypeId::UTINYINT:
		type_width = sizeof(uint8_t);
		break;
	case LogicalTypeId::USMALLINT:
		type_width = sizeof(uint16_t);
		break;
	case LogicalTypeId::UINTEGER:
		type_width = sizeof(uint32_t);
		break;
	case LogicalTypeId::UBIGINT:
		type_width = sizeof(uint64_t);
		break;
	case LogicalTypeId::TINYINT:
		type_width = sizeof(int8_t);
		break;
	case LogicalTypeId::SMALLINT:
		type_width = sizeof(int16_t);
		break;
	case LogicalTypeId::INTEGER:
		type_width = sizeof(int32_t);
		break;
	case LogicalTypeId::BIGINT:
		type_width = sizeof(int64_t);
		break;
	case LogicalTypeId::HUGEINT:
		type_width = sizeof(double);
		break;
	case LogicalTypeId::FLOAT:
		type_width = sizeof(float);
		break;
	case LogicalTypeId::DOUBLE:
		type_width = sizeof(double);
		break;
	case LogicalTypeId::DECIMAL:
		type_width = sizeof(double);
		break;
	case LogicalTypeId::TIMESTAMP:
		type_width = sizeof(int64_t);
		break;
	case LogicalTypeId::DATE:
		type_width = sizeof(int64_t);
		break;
	case LogicalTypeId::TIME:
		type_width = sizeof(PyObject *);
		break;
	case LogicalTypeId::VARCHAR:
		type_width = sizeof(PyObject *);
		break;
	case LogicalTypeId::BLOB:
		type_width = sizeof(PyObject *);
		break;
	default:
		throw std::runtime_error("Unsupported type " + type.ToString() + " for DuckDB -> NumPy conversion");
	}
}

void RawArrayWrapper::Initialize(idx_t capacity) {
	string dtype;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		dtype = "bool";
		break;
	case LogicalTypeId::TINYINT:
		dtype = "int8";
		break;
	case LogicalTypeId::SMALLINT:
		dtype = "int16";
		break;
	case LogicalTypeId::INTEGER:
		dtype = "int32";
		break;
	case LogicalTypeId::BIGINT:
		dtype = "int64";
		break;
	case LogicalTypeId::UTINYINT:
		dtype = "uint8";
		break;
	case LogicalTypeId::USMALLINT:
		dtype = "uint16";
		break;
	case LogicalTypeId::UINTEGER:
		dtype = "uint32";
		break;
	case LogicalTypeId::UBIGINT:
		dtype = "uint64";
		break;
	case LogicalTypeId::FLOAT:
		dtype = "float32";
		break;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		dtype = "float64";
		break;
	case LogicalTypeId::TIMESTAMP:
		dtype = "datetime64[ns]";
		break;
	case LogicalTypeId::DATE:
		dtype = "datetime64[ns]";
		break;
	case LogicalTypeId::TIME:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		dtype = "object";
		break;
	default:
		throw std::runtime_error("unsupported type " + type.ToString());
	}
	array = py::array(py::dtype(dtype), capacity);
	data = (data_ptr_t)array.mutable_data();
}

void RawArrayWrapper::Resize(idx_t new_capacity) {
	vector<ssize_t> new_shape {ssize_t(new_capacity)};
	array.resize(new_shape, false);
	data = (data_ptr_t)array.mutable_data();
}

ArrayWrapper::ArrayWrapper(const LogicalType &type) : requires_mask(false) {
	data = make_unique<RawArrayWrapper>(type);
	mask = make_unique<RawArrayWrapper>(LogicalType::BOOLEAN);
}

void ArrayWrapper::Initialize(idx_t capacity) {
	data->Initialize(capacity);
	mask->Initialize(capacity);
}

void ArrayWrapper::Resize(idx_t new_capacity) {
	data->Resize(new_capacity);
	mask->Resize(new_capacity);
}

void ArrayWrapper::Append(idx_t current_offset, Vector &input, idx_t count) {
	auto dataptr = data->data;
	auto maskptr = (bool *)mask->data;
	D_ASSERT(dataptr);
	D_ASSERT(maskptr);
	D_ASSERT(input.GetType() == data->type);
	bool may_have_null;

	VectorData idata;
	input.Orrify(count, idata);
	switch (input.GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		may_have_null = ConvertColumnRegular<bool>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::TINYINT:
		may_have_null = ConvertColumnRegular<int8_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::SMALLINT:
		may_have_null = ConvertColumnRegular<int16_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::INTEGER:
		may_have_null = ConvertColumnRegular<int32_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::BIGINT:
		may_have_null = ConvertColumnRegular<int64_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::UTINYINT:
		may_have_null = ConvertColumnRegular<uint8_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::USMALLINT:
		may_have_null = ConvertColumnRegular<uint16_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::UINTEGER:
		may_have_null = ConvertColumnRegular<uint32_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::UBIGINT:
		may_have_null = ConvertColumnRegular<uint64_t>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::HUGEINT:
		may_have_null = ConvertColumn<hugeint_t, double, duckdb_py_convert::IntegralConvert>(current_offset, dataptr,
		                                                                                     maskptr, idata, count);
		break;
	case LogicalTypeId::FLOAT:
		may_have_null = ConvertColumnRegular<float>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::DOUBLE:
		may_have_null = ConvertColumnRegular<double>(current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::DECIMAL:
		may_have_null = ConvertDecimal(input.GetType(), current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::TIMESTAMP:
		may_have_null = ConvertColumn<timestamp_t, int64_t, duckdb_py_convert::TimestampConvert>(
		    current_offset, dataptr, maskptr, idata, count);
		break;
	case LogicalTypeId::DATE:
		may_have_null = ConvertColumn<date_t, int64_t, duckdb_py_convert::DateConvert>(current_offset, dataptr, maskptr,
		                                                                               idata, count);
		break;
	case LogicalTypeId::TIME:
		may_have_null = ConvertColumn<dtime_t, PyObject *, duckdb_py_convert::TimeConvert>(current_offset, dataptr,
		                                                                                   maskptr, idata, count);
		break;
	case LogicalTypeId::VARCHAR:
		may_have_null = ConvertColumn<string_t, PyObject *, duckdb_py_convert::StringConvert>(current_offset, dataptr,
		                                                                                      maskptr, idata, count);
		break;
	case LogicalTypeId::BLOB:
		may_have_null = ConvertColumn<string_t, PyObject *, duckdb_py_convert::BlobConvert>(current_offset, dataptr,
		                                                                                    maskptr, idata, count);
		break;
	default:
		throw std::runtime_error("unsupported type " + input.GetType().ToString());
	}
	if (may_have_null) {
		requires_mask = true;
	}
	data->count += count;
	mask->count += count;
}

py::object ArrayWrapper::ToArray(idx_t count) const {
	D_ASSERT(data->array && mask->array);
	data->Resize(data->count);
	if (!requires_mask) {
		return move(data->array);
	}
	mask->Resize(mask->count);
	// construct numpy arrays from the data and the mask
	auto values = move(data->array);
	auto nullmask = move(mask->array);

	// create masked array and return it
	auto masked_array = py::module::import("numpy.ma").attr("masked_array")(values, nullmask);
	return masked_array;
}

NumpyResultConversion::NumpyResultConversion(vector<LogicalType> &types, idx_t initial_capacity)
    : count(0), capacity(0) {
	owned_data.reserve(types.size());
	for (auto &type : types) {
		owned_data.emplace_back(type);
	}
	Resize(initial_capacity);
}

void NumpyResultConversion::Resize(idx_t new_capacity) {
	if (capacity == 0) {
		for (auto &data : owned_data) {
			data.Initialize(new_capacity);
		}
	} else {
		for (auto &data : owned_data) {
			data.Resize(new_capacity);
		}
	}
	capacity = new_capacity;
}

void NumpyResultConversion::Append(DataChunk &chunk) {
	if (count + chunk.size() > capacity) {
		Resize(capacity * 2);
	}
	for (idx_t col_idx = 0; col_idx < owned_data.size(); col_idx++) {
		owned_data[col_idx].Append(count, chunk.data[col_idx], chunk.size());
	}
	count += chunk.size();
	for (auto &data : owned_data) {
		D_ASSERT(data.data->count == count);
		D_ASSERT(data.mask->count == count);
	}
}

namespace random_string {
static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_int_distribution<> dis(0, 15);

std::string Generate() {
	std::stringstream ss;
	int i;
	ss << std::hex;
	for (i = 0; i < 16; i++) {
		ss << dis(gen);
	}
	return ss.str();
}
} // namespace random_string

enum class PandasType : uint8_t {
	BOOLEAN,
	TINYINT,
	SMALLINT,
	INTEGER,
	BIGINT,
	UTINYINT,
	USMALLINT,
	UINTEGER,
	UBIGINT,
	FLOAT,
	DOUBLE,
	TIMESTAMP,
	VARCHAR
};

struct NumPyArrayWrapper {
	explicit NumPyArrayWrapper(py::array numpy_array) : numpy_array(move(numpy_array)) {
	}

	py::array numpy_array;
};

struct PandasColumnBindData {
	PandasType pandas_type;
	py::array numpy_col;
	unique_ptr<NumPyArrayWrapper> mask;
};

struct PandasScanFunctionData : public TableFunctionData {
	PandasScanFunctionData(py::handle df, idx_t row_count, vector<PandasColumnBindData> pandas_bind_data,
	                       vector<LogicalType> sql_types)
	    : df(df), row_count(row_count), lines_read(0), pandas_bind_data(move(pandas_bind_data)),
	      sql_types(move(sql_types)) {
	}
	py::handle df;
	idx_t row_count;
	std::atomic<idx_t> lines_read;
	vector<PandasColumnBindData> pandas_bind_data;
	vector<LogicalType> sql_types;
};

struct PandasScanState : public FunctionOperatorData {
	PandasScanState(idx_t start, idx_t end) : start(start), end(end) {
	}

	idx_t start;
	idx_t end;
	vector<column_t> column_ids;
};

struct ParallelPandasScanState : public ParallelState {
	ParallelPandasScanState() : position(0) {
	}

	std::mutex lock;
	idx_t position;
};

struct PandasScanFunction : public TableFunction {
	PandasScanFunction()
	    : TableFunction("pandas_scan", {LogicalType::VARCHAR}, PandasScanFunc, PandasScanBind, PandasScanInit, nullptr,
	                    nullptr, nullptr, PandasScanCardinality, nullptr, nullptr, PandasScanMaxThreads,
	                    PandasScanInitParallelState, PandasScanParallelInit, PandasScanParallelStateNext, true, false,
	                    PandasProgress) {
	}

	static void ConvertPandasType(const string &col_type, LogicalType &duckdb_col_type, PandasType &pandas_type) {
		if (col_type == "bool") {
			duckdb_col_type = LogicalType::BOOLEAN;
			pandas_type = PandasType::BOOLEAN;
		} else if (col_type == "uint8" || col_type == "Uint8") {
			duckdb_col_type = LogicalType::UTINYINT;
			pandas_type = PandasType::UTINYINT;
		} else if (col_type == "uint16" || col_type == "Uint16") {
			duckdb_col_type = LogicalType::USMALLINT;
			pandas_type = PandasType::USMALLINT;
		} else if (col_type == "uint32" || col_type == "Uint32") {
			duckdb_col_type = LogicalType::UINTEGER;
			pandas_type = PandasType::UINTEGER;
		} else if (col_type == "uint64" || col_type == "Uint64") {
			duckdb_col_type = LogicalType::UBIGINT;
			pandas_type = PandasType::UBIGINT;
		} else if (col_type == "int8" || col_type == "Int8") {
			duckdb_col_type = LogicalType::TINYINT;
			pandas_type = PandasType::TINYINT;
		} else if (col_type == "int16" || col_type == "Int16") {
			duckdb_col_type = LogicalType::SMALLINT;
			pandas_type = PandasType::SMALLINT;
		} else if (col_type == "int32" || col_type == "Int32") {
			duckdb_col_type = LogicalType::INTEGER;
			pandas_type = PandasType::INTEGER;
		} else if (col_type == "int64" || col_type == "Int64") {
			duckdb_col_type = LogicalType::BIGINT;
			pandas_type = PandasType::BIGINT;
		} else if (col_type == "float32") {
			duckdb_col_type = LogicalType::FLOAT;
			pandas_type = PandasType::FLOAT;
		} else if (col_type == "float64") {
			duckdb_col_type = LogicalType::DOUBLE;
			pandas_type = PandasType::DOUBLE;
		} else if (col_type == "object" || col_type == "string") {
			// this better be strings
			duckdb_col_type = LogicalType::VARCHAR;
			pandas_type = PandasType::VARCHAR;
		} else {
			throw std::runtime_error("unsupported python type " + col_type);
		}
	}

	static unique_ptr<FunctionData> PandasScanBind(ClientContext &context, vector<Value> &inputs,
	                                               unordered_map<string, Value> &named_parameters,
	                                               vector<LogicalType> &return_types, vector<string> &names) {
		// Hey, it works (TM)
		py::gil_scoped_acquire acquire;
		py::handle df((PyObject *)std::stoull(inputs[0].GetValue<string>(), nullptr, 16));

		/* TODO this fails on Python2 for some reason
		auto pandas_mod = py::module::import("pandas.core.frame");
		auto df_class = pandas_mod.attr("DataFrame");

		if (!df.get_type().is(df_class)) {
		    throw Exception("parameter is not a DataFrame");
		} */

		auto df_columns = py::list(df.attr("columns"));
		auto df_types = py::list(df.attr("dtypes"));
		auto get_fun = df.attr("__getitem__");
		// TODO support masked arrays as well
		// TODO support dicts of numpy arrays as well
		if (py::len(df_columns) == 0 || py::len(df_types) == 0 || py::len(df_columns) != py::len(df_types)) {
			throw std::runtime_error("Need a DataFrame with at least one column");
		}
		vector<PandasColumnBindData> pandas_bind_data;
		for (idx_t col_idx = 0; col_idx < py::len(df_columns); col_idx++) {
			LogicalType duckdb_col_type;
			PandasColumnBindData bind_data;

			auto col_type = string(py::str(df_types[col_idx]));
			if (col_type == "Int8" || col_type == "Int16" || col_type == "Int32" || col_type == "Int64") {
				// numeric object
				// fetch the internal data and mask array
				bind_data.numpy_col = get_fun(df_columns[col_idx]).attr("array").attr("_data");
				bind_data.mask =
				    make_unique<NumPyArrayWrapper>(get_fun(df_columns[col_idx]).attr("array").attr("_mask"));
				ConvertPandasType(col_type, duckdb_col_type, bind_data.pandas_type);
			} else if (StringUtil::StartsWith(col_type, "datetime64[ns") || col_type == "<M8[ns]") {
				// timestamp type
				bind_data.numpy_col = get_fun(df_columns[col_idx]).attr("array").attr("_data");
				bind_data.mask = nullptr;
				duckdb_col_type = LogicalType::TIMESTAMP;
				bind_data.pandas_type = PandasType::TIMESTAMP;
			} else {
				// regular type
				auto column = get_fun(df_columns[col_idx]);
				bind_data.numpy_col = py::array(column.attr("to_numpy")());
				bind_data.mask = nullptr;
				if (col_type == "category") {
					// for category types, we use the converted numpy type
					auto numpy_type = bind_data.numpy_col.attr("dtype");
					auto category_type = string(py::str(numpy_type));
					ConvertPandasType(category_type, duckdb_col_type, bind_data.pandas_type);
				} else {
					ConvertPandasType(col_type, duckdb_col_type, bind_data.pandas_type);
				}
			}
			names.emplace_back(py::str(df_columns[col_idx]));
			return_types.push_back(duckdb_col_type);
			pandas_bind_data.push_back(move(bind_data));
		}
		idx_t row_count = py::len(get_fun(df_columns[0]));
		return make_unique<PandasScanFunctionData>(df, row_count, move(pandas_bind_data), return_types);
	}

	static unique_ptr<FunctionOperatorData> PandasScanInit(ClientContext &context, const FunctionData *bind_data_p,
	                                                       vector<column_t> &column_ids,
	                                                       TableFilterCollection *filters) {
		auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
		auto result = make_unique<PandasScanState>(0, bind_data.row_count);
		result->column_ids = column_ids;
		return result;
	}

	static constexpr idx_t PANDAS_PARTITION_COUNT = 50 * STANDARD_VECTOR_SIZE;

	static idx_t PandasScanMaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
		auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
		return bind_data.row_count / PANDAS_PARTITION_COUNT + 1;
	}

	static unique_ptr<ParallelState> PandasScanInitParallelState(ClientContext &context,
	                                                             const FunctionData *bind_data_p) {
		return make_unique<ParallelPandasScanState>();
	}

	static unique_ptr<FunctionOperatorData> PandasScanParallelInit(ClientContext &context,
	                                                               const FunctionData *bind_data_p,
	                                                               ParallelState *state, vector<column_t> &column_ids,
	                                                               TableFilterCollection *filters) {
		auto result = make_unique<PandasScanState>(0, 0);
		result->column_ids = column_ids;
		if (!PandasScanParallelStateNext(context, bind_data_p, result.get(), state)) {
			return nullptr;
		}
		return move(result);
	}

	static bool PandasScanParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
	                                        FunctionOperatorData *operator_state, ParallelState *parallel_state_p) {
		auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
		auto &parallel_state = (ParallelPandasScanState &)*parallel_state_p;
		auto &state = (PandasScanState &)*operator_state;

		lock_guard<mutex> parallel_lock(parallel_state.lock);
		if (parallel_state.position >= bind_data.row_count) {
			return false;
		}
		state.start = parallel_state.position;
		parallel_state.position += PANDAS_PARTITION_COUNT;
		if (parallel_state.position > bind_data.row_count) {
			parallel_state.position = bind_data.row_count;
		}
		state.end = parallel_state.position;
		return true;
	}

	static int PandasProgress(ClientContext &context, const FunctionData *bind_data_p) {
		auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
		if (bind_data.row_count == 0) {
			return 100;
		}
		auto percentage = bind_data.lines_read * 100 / bind_data.row_count;
		return percentage;
	}

	template <class T>
	static void ScanPandasColumn(py::array &numpy_col, idx_t count, idx_t offset, Vector &out) {
		auto src_ptr = (T *)numpy_col.data();
		FlatVector::SetData(out, (data_ptr_t)(src_ptr + offset));
	}

	template <class T>
	static void ScanPandasNumeric(PandasColumnBindData &bind_data, idx_t count, idx_t offset, Vector &out) {
		ScanPandasColumn<T>(bind_data.numpy_col, count, offset, out);
		if (bind_data.mask) {
			auto mask = (bool *)bind_data.mask->numpy_array.data();
			for (idx_t i = 0; i < count; i++) {
				auto is_null = mask[offset + i];
				if (is_null) {
					FlatVector::SetNull(out, i, true);
				}
			}
		}
	}

	template <class T>
	static bool ValueIsNull(T value) {
		throw std::runtime_error("unsupported type for ValueIsNull");
	}

	template <class T>
	static void ScanPandasFpColumn(T *src_ptr, idx_t count, idx_t offset, Vector &out) {
		FlatVector::SetData(out, (data_ptr_t)(src_ptr + offset));
		auto tgt_ptr = FlatVector::GetData<T>(out);
		auto &mask = FlatVector::Validity(out);
		for (idx_t i = 0; i < count; i++) {
			if (ValueIsNull(tgt_ptr[i])) {
				mask.SetInvalid(i);
			}
		}
	}

	template <class T>
	static string_t DecodePythonUnicode(T *codepoints, idx_t codepoint_count, Vector &out) {
		// first figure out how many bytes to allocate
		idx_t utf8_length = 0;
		for (idx_t i = 0; i < codepoint_count; i++) {
			int len = Utf8Proc::CodepointLength(int(codepoints[i]));
			D_ASSERT(len >= 1);
			utf8_length += len;
		}
		int sz;
		auto result = StringVector::EmptyString(out, utf8_length);
		auto target = result.GetDataWriteable();
		for (idx_t i = 0; i < codepoint_count; i++) {
			Utf8Proc::CodepointToUtf8(int(codepoints[i]), sz, target);
			D_ASSERT(sz >= 1);
			target += sz;
		}
		return result;
	}

	static void ConvertVector(PandasColumnBindData &bind_data, py::array &numpy_col, idx_t count, idx_t offset,
	                          Vector &out) {
		switch (bind_data.pandas_type) {
		case PandasType::BOOLEAN:
			ScanPandasColumn<bool>(numpy_col, count, offset, out);
			break;
		case PandasType::UTINYINT:
			ScanPandasNumeric<uint8_t>(bind_data, count, offset, out);
			break;
		case PandasType::USMALLINT:
			ScanPandasNumeric<uint16_t>(bind_data, count, offset, out);
			break;
		case PandasType::UINTEGER:
			ScanPandasNumeric<uint32_t>(bind_data, count, offset, out);
			break;
		case PandasType::UBIGINT:
			ScanPandasNumeric<uint64_t>(bind_data, count, offset, out);
			break;
		case PandasType::TINYINT:
			ScanPandasNumeric<int8_t>(bind_data, count, offset, out);
			break;
		case PandasType::SMALLINT:
			ScanPandasNumeric<int16_t>(bind_data, count, offset, out);
			break;
		case PandasType::INTEGER:
			ScanPandasNumeric<int32_t>(bind_data, count, offset, out);
			break;
		case PandasType::BIGINT:
			ScanPandasNumeric<int64_t>(bind_data, count, offset, out);
			break;
		case PandasType::FLOAT:
			ScanPandasFpColumn<float>((float *)numpy_col.data(), count, offset, out);
			break;
		case PandasType::DOUBLE:
			ScanPandasFpColumn<double>((double *)numpy_col.data(), count, offset, out);
			break;
		case PandasType::TIMESTAMP: {
			auto src_ptr = (int64_t *)numpy_col.data();
			auto tgt_ptr = FlatVector::GetData<timestamp_t>(out);
			auto &mask = FlatVector::Validity(out);

			for (idx_t row = 0; row < count; row++) {
				auto source_idx = offset + row;
				if (src_ptr[source_idx] <= NumericLimits<int64_t>::Minimum()) {
					// pandas Not a Time (NaT)
					mask.SetInvalid(row);
					continue;
				}
				tgt_ptr[row] = Timestamp::FromEpochNanoSeconds(src_ptr[source_idx]);
			}
			break;
		}
		case PandasType::VARCHAR: {
			auto src_ptr = (PyObject **)numpy_col.data();
			auto tgt_ptr = FlatVector::GetData<string_t>(out);
			for (idx_t row = 0; row < count; row++) {
				auto source_idx = offset + row;
				auto val = src_ptr[source_idx];
#if PY_MAJOR_VERSION >= 3
				// Python 3 string representation:
				// https://github.com/python/cpython/blob/3a8fdb28794b2f19f6c8464378fb8b46bce1f5f4/Include/cpython/unicodeobject.h#L79
				if (!PyUnicode_CheckExact(val)) {
					FlatVector::SetNull(out, row, true);
					continue;
				}
				if (PyUnicode_IS_COMPACT_ASCII(val)) {
					// ascii string: we can zero copy
					tgt_ptr[row] = string_t((const char *)PyUnicode_DATA(val), PyUnicode_GET_LENGTH(val));
				} else {
					// unicode gunk
					auto ascii_obj = (PyASCIIObject *)val;
					auto unicode_obj = (PyCompactUnicodeObject *)val;
					// compact unicode string: is there utf8 data available?
					if (unicode_obj->utf8) {
						// there is! zero copy
						tgt_ptr[row] = string_t((const char *)unicode_obj->utf8, unicode_obj->utf8_length);
					} else if (PyUnicode_IS_COMPACT(unicode_obj) && !PyUnicode_IS_ASCII(unicode_obj)) {
						auto kind = PyUnicode_KIND(val);
						switch (kind) {
						case PyUnicode_1BYTE_KIND:
							tgt_ptr[row] =
							    DecodePythonUnicode<Py_UCS1>(PyUnicode_1BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
							break;
						case PyUnicode_2BYTE_KIND:
							tgt_ptr[row] =
							    DecodePythonUnicode<Py_UCS2>(PyUnicode_2BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
							break;
						case PyUnicode_4BYTE_KIND:
							tgt_ptr[row] =
							    DecodePythonUnicode<Py_UCS4>(PyUnicode_4BYTE_DATA(val), PyUnicode_GET_LENGTH(val), out);
							break;
						default:
							throw std::runtime_error("Unsupported typekind for Python Unicode Compact decode");
						}
					} else if (ascii_obj->state.kind == PyUnicode_WCHAR_KIND) {
						throw std::runtime_error("Unsupported: decode not ready legacy string");
					} else if (!PyUnicode_IS_COMPACT(unicode_obj) && ascii_obj->state.kind != PyUnicode_WCHAR_KIND) {
						throw std::runtime_error("Unsupported: decode ready legacy string");
					} else {
						throw std::runtime_error("Unsupported string type: no clue what this string is");
					}
				}
#else
				if (PyString_CheckExact(val)) {
					auto dataptr = PyString_AS_STRING(val);
					auto size = PyString_GET_SIZE(val);
					// string object: directly pass the data
					if (Utf8Proc::Analyze(dataptr, size) == UnicodeType::INVALID) {
						throw std::runtime_error("String does contains invalid UTF8! Please encode as UTF8 first");
					}
					tgt_ptr[row] = string_t(dataptr, uint32_t(size));
				} else if (PyUnicode_CheckExact(val)) {
					throw std::runtime_error("Unicode is only supported in Python 3 and up.");
				} else {
					FlatVector::SetNull(out, row, true);
					continue;
				}
#endif
			}
			break;
		}
		default:
			throw std::runtime_error("Unsupported type " + out.GetType().ToString());
		}
	}

	//! The main pandas scan function: note that this can be called in parallel without the GIL
	//! hence this needs to be GIL-safe, i.e. no methods that create Python objects are allowed
	static void PandasScanFunc(ClientContext &context, const FunctionData *bind_data,
	                           FunctionOperatorData *operator_state, DataChunk &output) {
		auto &data = (PandasScanFunctionData &)*bind_data;
		auto &state = (PandasScanState &)*operator_state;

		if (state.start >= state.end) {
			return;
		}
		idx_t this_count = std::min((idx_t)STANDARD_VECTOR_SIZE, state.end - state.start);
		output.SetCardinality(this_count);
		for (idx_t idx = 0; idx < state.column_ids.size(); idx++) {
			auto col_idx = state.column_ids[idx];
			if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
				output.data[idx].Sequence(state.start, this_count);
			} else {
				ConvertVector(data.pandas_bind_data[col_idx], data.pandas_bind_data[col_idx].numpy_col, this_count,
				              state.start, output.data[idx]);
			}
		}
		state.start += this_count;
		data.lines_read += this_count;
	}

	static unique_ptr<NodeStatistics> PandasScanCardinality(ClientContext &context, const FunctionData *bind_data) {
		auto &data = (PandasScanFunctionData &)*bind_data;
		return make_unique<NodeStatistics>(data.row_count, data.row_count);
	}
};

template <>
bool PandasScanFunction::ValueIsNull(float value);
template <>
bool PandasScanFunction::ValueIsNull(double value);

template <>
bool PandasScanFunction::ValueIsNull(float value) {
	return !Value::FloatIsValid(value);
}

template <>
bool PandasScanFunction::ValueIsNull(double value) {
	return !Value::DoubleIsValid(value);
}

struct DuckDBPyResult {
public:
	idx_t chunk_offset = 0;

	unique_ptr<QueryResult> result;
	unique_ptr<DataChunk> current_chunk;

public:
	template <class SRC>
	static SRC FetchScalar(Vector &src_vec, idx_t offset) {
		auto src_ptr = FlatVector::GetData<SRC>(src_vec);
		return src_ptr[offset];
	}

	py::object Fetchone() {
		if (!result) {
			throw std::runtime_error("result closed");
		}
		if (!current_chunk || chunk_offset >= current_chunk->size()) {
			current_chunk = result->Fetch();
			chunk_offset = 0;
		}
		if (!current_chunk || current_chunk->size() == 0) {
			return py::none();
		}
		py::tuple res(result->types.size());

		for (idx_t col_idx = 0; col_idx < result->types.size(); col_idx++) {
			auto &mask = FlatVector::Validity(current_chunk->data[col_idx]);
			if (!mask.RowIsValid(chunk_offset)) {
				res[col_idx] = py::none();
				continue;
			}
			auto val = current_chunk->data[col_idx].GetValue(chunk_offset);
			switch (result->types[col_idx].id()) {
			case LogicalTypeId::BOOLEAN:
				res[col_idx] = val.GetValue<bool>();
				break;
			case LogicalTypeId::TINYINT:
				res[col_idx] = val.GetValue<int8_t>();
				break;
			case LogicalTypeId::SMALLINT:
				res[col_idx] = val.GetValue<int16_t>();
				break;
			case LogicalTypeId::INTEGER:
				res[col_idx] = val.GetValue<int32_t>();
				break;
			case LogicalTypeId::BIGINT:
				res[col_idx] = val.GetValue<int64_t>();
				break;
			case LogicalTypeId::UTINYINT:
				res[col_idx] = val.GetValue<uint8_t>();
				break;
			case LogicalTypeId::USMALLINT:
				res[col_idx] = val.GetValue<uint16_t>();
				break;
			case LogicalTypeId::UINTEGER:
				res[col_idx] = val.GetValue<uint32_t>();
				break;
			case LogicalTypeId::UBIGINT:
				res[col_idx] = val.GetValue<uint64_t>();
				break;
			case LogicalTypeId::HUGEINT: {
				auto hugeint_str = val.GetValue<string>();
				res[col_idx] = PyLong_FromString((char *)hugeint_str.c_str(), nullptr, 10);
				break;
			}
			case LogicalTypeId::FLOAT:
				res[col_idx] = val.GetValue<float>();
				break;
			case LogicalTypeId::DOUBLE:
				res[col_idx] = val.GetValue<double>();
				break;
			case LogicalTypeId::DECIMAL:
				res[col_idx] = val.CastAs(LogicalType::DOUBLE).GetValue<double>();
				break;
			case LogicalTypeId::VARCHAR:
				res[col_idx] = val.GetValue<string>();
				break;
			case LogicalTypeId::BLOB:
				res[col_idx] = py::bytes(val.GetValue<string>());
				break;
			case LogicalTypeId::TIMESTAMP: {
				D_ASSERT(result->types[col_idx].InternalType() == PhysicalType::INT64);

				auto timestamp = val.GetValueUnsafe<int64_t>();
				int32_t year, month, day, hour, min, sec, micros;
				date_t date;
				dtime_t time;
				Timestamp::Convert(timestamp, date, time);
				Date::Convert(date, year, month, day);
				Time::Convert(time, hour, min, sec, micros);
				res[col_idx] = PyDateTime_FromDateAndTime(year, month, day, hour, min, sec, micros);
				break;
			}
			case LogicalTypeId::TIME: {
				D_ASSERT(result->types[col_idx].InternalType() == PhysicalType::INT64);

				int32_t hour, min, sec, microsec;
				auto time = val.GetValueUnsafe<int64_t>();
				duckdb::Time::Convert(time, hour, min, sec, microsec);
				res[col_idx] = PyTime_FromTime(hour, min, sec, microsec);
				break;
			}
			case LogicalTypeId::DATE: {
				D_ASSERT(result->types[col_idx].InternalType() == PhysicalType::INT32);

				auto date = val.GetValueUnsafe<int32_t>();
				int32_t year, month, day;
				duckdb::Date::Convert(date, year, month, day);
				res[col_idx] = PyDate_FromDate(year, month, day);
				break;
			}

			default:
				throw std::runtime_error("unsupported type: " + result->types[col_idx].ToString());
			}
		}
		chunk_offset++;
		return move(res);
	}

	py::list Fetchall() {
		py::list res;
		while (true) {
			auto fres = Fetchone();
			if (fres.is_none()) {
				break;
			}
			res.append(fres);
		}
		return res;
	}

	py::dict FetchNumpy(bool stream = false) {
		if (!result) {
			throw std::runtime_error("result closed");
		}

		// iterate over the result to materialize the data needed for the NumPy arrays
		idx_t initial_capacity = STANDARD_VECTOR_SIZE * 2;
		if (result->type == QueryResultType::MATERIALIZED_RESULT) {
			// materialized query result: we know exactly how much space we need
			auto &materialized = (MaterializedQueryResult &)*result;
			initial_capacity = materialized.collection.Count();
		}

		NumpyResultConversion conversion(result->types, initial_capacity);
		if (result->type == QueryResultType::MATERIALIZED_RESULT) {
			auto &materialized = (MaterializedQueryResult &)*result;
			if (!stream) {
				for (auto &chunk : materialized.collection.Chunks()) {
					conversion.Append(*chunk);
				}
				materialized.collection.Reset();
			} else {
				conversion.Append(*materialized.Fetch());
			}
		} else {
			if (!stream) {
				while (true) {
					auto chunk = result->FetchRaw();
					if (!chunk || chunk->size() == 0) {
						// finished
						break;
					}
					conversion.Append(*chunk);
				}
			} else {
				auto chunk = result->FetchRaw();
				conversion.Append(*chunk);
			}
		}

		// now that we have materialized the result in contiguous arrays, construct the actual NumPy arrays
		py::dict res;
		for (idx_t col_idx = 0; col_idx < result->types.size(); col_idx++) {
			res[result->names[col_idx].c_str()] = conversion.ToArray(col_idx);
		}
		return res;
	}

	py::object FetchDF() {
		return py::module::import("pandas").attr("DataFrame").attr("from_dict")(FetchNumpy());
	}

	py::object FetchDFChunk() {
		return py::module::import("pandas").attr("DataFrame").attr("from_dict")(FetchNumpy(true));
	}

	py::object FetchArrowTable() {
		if (!result) {
			throw std::runtime_error("result closed");
		}

		auto pyarrow_lib_module = py::module::import("pyarrow").attr("lib");

		auto batch_import_func = pyarrow_lib_module.attr("RecordBatch").attr("_import_from_c");
		auto from_batches_func = pyarrow_lib_module.attr("Table").attr("from_batches");
		auto schema_import_func = pyarrow_lib_module.attr("Schema").attr("_import_from_c");
		ArrowSchema schema;
		result->ToArrowSchema(&schema);
		auto schema_obj = schema_import_func((uint64_t)&schema);

		py::list batches;
		while (true) {
			auto data_chunk = result->Fetch();
			if (!data_chunk || data_chunk->size() == 0) {
				break;
			}
			ArrowArray data;
			data_chunk->ToArrowArray(&data);
			ArrowSchema arrow_schema;
			result->ToArrowSchema(&arrow_schema);
			batches.append(batch_import_func((uint64_t)&data, (uint64_t)&arrow_schema));
		}
		return from_batches_func(batches, schema_obj);
	}

	py::list Description() {
		py::list desc(result->names.size());
		for (idx_t col_idx = 0; col_idx < result->names.size(); col_idx++) {
			py::tuple col_desc(7);
			col_desc[0] = py::str(result->names[col_idx]);
			col_desc[1] = py::none();
			col_desc[2] = py::none();
			col_desc[3] = py::none();
			col_desc[4] = py::none();
			col_desc[5] = py::none();
			col_desc[6] = py::none();
			desc[col_idx] = col_desc;
		}
		return desc;
	}

	void Close() {
		result = nullptr;
	}
};

struct DuckDBPyRelation;

struct DuckDBPyConnection {
	DuckDBPyConnection *ExecuteMany(const string &query, py::object params = py::list()) {
		Execute(query, std::move(params), true);
		return this;
	}

	~DuckDBPyConnection() {
		for (auto &element : registered_dfs) {
			UnregisterDF(element.first);
		}
	}

	DuckDBPyConnection *Execute(const string &query, py::object params = py::list(), bool many = false) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		result = nullptr;

		auto statements = connection->ExtractStatements(query);
		if (statements.empty()) {
			// no statements to execute
			return this;
		}
		// if there are multiple statements, we directly execute the statements besides the last one
		// we only return the result of the last statement to the user, unless one of the previous statements fails
		for (idx_t i = 0; i + 1 < statements.size(); i++) {
			auto res = connection->Query(move(statements[i]));
			if (!res->success) {
				throw std::runtime_error(res->error);
			}
		}

		auto prep = connection->Prepare(move(statements.back()));
		if (!prep->success) {
			throw std::runtime_error(prep->error);
		}

		// this is a list of a list of parameters in executemany
		py::list params_set;
		if (!many) {
			params_set = py::list(1);
			params_set[0] = params;
		} else {
			params_set = params;
		}

		for (pybind11::handle single_query_params : params_set) {
			if (prep->n_param != py::len(single_query_params)) {
				throw std::runtime_error("Prepared statement needs " + to_string(prep->n_param) + " parameters, " +
				                         to_string(py::len(single_query_params)) + " given");
			}
			auto args = DuckDBPyConnection::TransformPythonParamList(single_query_params);
			auto res = make_unique<DuckDBPyResult>();
			{
				py::gil_scoped_release release;
				res->result = prep->Execute(args);
			}
			if (!res->result->success) {
				throw std::runtime_error(res->result->error);
			}
			if (!many) {
				result = move(res);
			}
		}
		return this;
	}

	DuckDBPyConnection *Append(const string &name, py::object value) {
		RegisterDF("__append_df", std::move(value));
		return Execute("INSERT INTO \"" + name + "\" SELECT * FROM __append_df");
	}

	static string PtrToString(void const *ptr) {
		std::ostringstream address;
		address << ptr;
		return address.str();
	}

	DuckDBPyConnection *RegisterDF(const string &name, py::object value) {
		// hack alert: put the pointer address into the function call as a string
		Execute("CREATE OR REPLACE VIEW \"" + name + "\" AS SELECT * FROM pandas_scan('" + PtrToString(value.ptr()) +
		        "')");

		// try to bind
		Execute("SELECT * FROM \"" + name + "\" WHERE FALSE");

		// keep a reference
		registered_dfs[name] = value;
		return this;
	}

	unique_ptr<DuckDBPyRelation> Table(const string &tname) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		return make_unique<DuckDBPyRelation>(connection->Table(tname));
	}

	unique_ptr<DuckDBPyRelation> Values(py::object params = py::list()) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		vector<vector<Value>> values {DuckDBPyConnection::TransformPythonParamList(std::move(params))};
		return make_unique<DuckDBPyRelation>(connection->Values(values));
	}

	unique_ptr<DuckDBPyRelation> View(const string &vname) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		return make_unique<DuckDBPyRelation>(connection->View(vname));
	}

	unique_ptr<DuckDBPyRelation> TableFunction(const string &fname, py::object params = py::list()) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}

		return make_unique<DuckDBPyRelation>(
		    connection->TableFunction(fname, DuckDBPyConnection::TransformPythonParamList(std::move(params))));
	}

	unique_ptr<DuckDBPyRelation> FromDF(py::object value) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		string name = "df_" + random_string::Generate();
		registered_dfs[name] = value;
		vector<Value> params;
		params.emplace_back(PtrToString(value.ptr()));
		return make_unique<DuckDBPyRelation>(connection->TableFunction("pandas_scan", params)->Alias(name));
	}

	unique_ptr<DuckDBPyRelation> FromCsvAuto(const string &filename) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		vector<Value> params;
		params.emplace_back(filename);
		return make_unique<DuckDBPyRelation>(connection->TableFunction("read_csv_auto", params)->Alias(filename));
	}

	unique_ptr<DuckDBPyRelation> FromParquet(const string &filename) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}
		vector<Value> params;
		params.emplace_back(filename);
		return make_unique<DuckDBPyRelation>(connection->TableFunction("parquet_scan", params)->Alias(filename));
	}

	struct PythonTableArrowArrayStream {
		explicit PythonTableArrowArrayStream(const py::object &arrow_table) : arrow_table(arrow_table) {
			stream.get_schema = PythonTableArrowArrayStream::MyStreamGetSchema;
			stream.get_next = PythonTableArrowArrayStream::MyStreamGetNext;
			stream.release = PythonTableArrowArrayStream::MyStreamRelease;
			stream.get_last_error = PythonTableArrowArrayStream::MyStreamGetLastError;
			stream.private_data = this;

			batches = arrow_table.attr("to_batches")();
		}

		static int MyStreamGetSchema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
			D_ASSERT(stream->private_data);
			auto my_stream = (PythonTableArrowArrayStream *)stream->private_data;
			if (!stream->release) {
				my_stream->last_error = "stream was released";
				return -1;
			}
			my_stream->arrow_table.attr("schema").attr("_export_to_c")((uint64_t)out);
			return 0;
		}

		static int MyStreamGetNext(struct ArrowArrayStream *stream, struct ArrowArray *out) {
			D_ASSERT(stream->private_data);
			auto my_stream = (PythonTableArrowArrayStream *)stream->private_data;
			if (!stream->release) {
				my_stream->last_error = "stream was released";
				return -1;
			}
			if (my_stream->batch_idx >= py::len(my_stream->batches)) {
				out->release = nullptr;
				return 0;
			}
			my_stream->batches[my_stream->batch_idx++].attr("_export_to_c")((uint64_t)out);
			return 0;
		}

		static void MyStreamRelease(struct ArrowArrayStream *stream) {
			if (!stream->release) {
				return;
			}
			stream->release = nullptr;
			delete (PythonTableArrowArrayStream *)stream->private_data;
		}

		static const char *MyStreamGetLastError(struct ArrowArrayStream *stream) {
			if (!stream->release) {
				return "stream was released";
			}
			D_ASSERT(stream->private_data);
			auto my_stream = (PythonTableArrowArrayStream *)stream->private_data;
			return my_stream->last_error.c_str();
		}

		ArrowArrayStream stream;
		string last_error;
		py::object arrow_table;
		py::list batches;
		idx_t batch_idx = 0;
	};

	unique_ptr<DuckDBPyRelation> FromArrowTable(const py::object &table) {
		if (!connection) {
			throw std::runtime_error("connection closed");
		}

		// the following is a careful dance around having to depend on pyarrow
		if (table.is_none() || string(py::str(table.get_type().attr("__name__"))) != "Table") {
			throw std::runtime_error("Only arrow tables supported");
		}

		auto my_arrow_table = new PythonTableArrowArrayStream(table);
		string name = "arrow_table_" + PtrToString((void *)my_arrow_table);
		return make_unique<DuckDBPyRelation>(
		    connection->TableFunction("arrow_scan", {Value::POINTER((uintptr_t)my_arrow_table)})->Alias(name));
	}

	DuckDBPyConnection *UnregisterDF(const string &name) {
		registered_dfs[name] = py::none();
		return this;
	}

	DuckDBPyConnection *Begin() {
		Execute("BEGIN TRANSACTION");
		return this;
	}

	DuckDBPyConnection *Commit() {
		if (connection->context->transaction.IsAutoCommit()) {
			return this;
		}
		Execute("COMMIT");
		return this;
	}

	DuckDBPyConnection *Rollback() {
		Execute("ROLLBACK");
		return this;
	}

	py::object GetAttr(const py::str &key) {
		if (key.cast<string>() == "description") {
			if (!result) {
				throw std::runtime_error("no open result set");
			}
			return result->Description();
		}
		return py::none();
	}

	void Close() {
		result = nullptr;
		connection = nullptr;
		database = nullptr;
		for (auto &cur : cursors) {
			cur->Close();
		}
		cursors.clear();
	}

	// cursor() is stupid
	shared_ptr<DuckDBPyConnection> Cursor() {
		auto res = make_shared<DuckDBPyConnection>();
		res->database = database;
		res->connection = make_unique<Connection>(*res->database);
		cursors.push_back(res);
		return res;
	}

	// these should be functions on the result but well
	py::object FetchOne() {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->Fetchone();
	}

	py::list FetchAll() {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->Fetchall();
	}

	py::dict FetchNumpy() {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->FetchNumpy();
	}
	py::object FetchDF() {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->FetchDF();
	}

	py::object FetchDFChunk() const {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->FetchDFChunk();
	}

	py::object FetchArrow() {
		if (!result) {
			throw std::runtime_error("no open result set");
		}
		return result->FetchArrowTable();
	}

	static shared_ptr<DuckDBPyConnection> Connect(const string &database, bool read_only) {
		auto res = make_shared<DuckDBPyConnection>();
		DBConfig config;
		if (read_only) {
			config.access_mode = AccessMode::READ_ONLY;
		}
		res->database = make_unique<DuckDB>(database, &config);
		ExtensionHelper::LoadAllExtensions(*res->database);
		res->connection = make_unique<Connection>(*res->database);

		PandasScanFunction scan_fun;
		CreateTableFunctionInfo info(scan_fun);

		auto &context = *res->connection->context;
		auto &catalog = Catalog::GetCatalog(context);
		context.transaction.BeginTransaction();
		catalog.CreateTableFunction(context, &info);
		context.transaction.Commit();

		return res;
	}

	shared_ptr<DuckDB> database;
	unique_ptr<Connection> connection;
	unordered_map<string, py::object> registered_dfs;
	unique_ptr<DuckDBPyResult> result;
	vector<shared_ptr<DuckDBPyConnection>> cursors;

	static vector<Value> TransformPythonParamList(py::handle params) {
		vector<Value> args;

		auto datetime_mod = py::module::import("datetime");
		auto datetime_date = datetime_mod.attr("date");
		auto datetime_datetime = datetime_mod.attr("datetime");
		auto datetime_time = datetime_mod.attr("time");
		auto decimal_mod = py::module::import("decimal");
		auto decimal_decimal = decimal_mod.attr("Decimal");

		for (pybind11::handle ele : params) {
			if (ele.is_none()) {
				args.emplace_back();
			} else if (py::isinstance<py::bool_>(ele)) {
				args.push_back(Value::BOOLEAN(ele.cast<bool>()));
			} else if (py::isinstance<py::int_>(ele)) {
				args.push_back(Value::BIGINT(ele.cast<int64_t>()));
			} else if (py::isinstance<py::float_>(ele)) {
				args.push_back(Value::DOUBLE(ele.cast<double>()));
			} else if (py::isinstance<py::str>(ele)) {
				args.emplace_back(ele.cast<string>());
			} else if (py::isinstance(ele, decimal_decimal)) {
				args.emplace_back(py::str(ele).cast<string>());
			} else if (py::isinstance(ele, datetime_datetime)) {
				auto year = PyDateTime_GET_YEAR(ele.ptr());
				auto month = PyDateTime_GET_MONTH(ele.ptr());
				auto day = PyDateTime_GET_DAY(ele.ptr());
				auto hour = PyDateTime_DATE_GET_HOUR(ele.ptr());
				auto minute = PyDateTime_DATE_GET_MINUTE(ele.ptr());
				auto second = PyDateTime_DATE_GET_SECOND(ele.ptr());
				auto micros = PyDateTime_DATE_GET_MICROSECOND(ele.ptr());
				args.push_back(Value::TIMESTAMP(year, month, day, hour, minute, second, micros));
			} else if (py::isinstance(ele, datetime_time)) {
				auto hour = PyDateTime_TIME_GET_HOUR(ele.ptr());
				auto minute = PyDateTime_TIME_GET_MINUTE(ele.ptr());
				auto second = PyDateTime_TIME_GET_SECOND(ele.ptr());
				auto micros = PyDateTime_TIME_GET_MICROSECOND(ele.ptr());
				args.push_back(Value::TIME(hour, minute, second, micros));
			} else if (py::isinstance(ele, datetime_date)) {
				auto year = PyDateTime_GET_YEAR(ele.ptr());
				auto month = PyDateTime_GET_MONTH(ele.ptr());
				auto day = PyDateTime_GET_DAY(ele.ptr());
				args.push_back(Value::DATE(year, month, day));
			} else {
				throw std::runtime_error("unknown param type " + py::str(ele.get_type()).cast<string>());
			}
		}
		return args;
	}
};

static shared_ptr<DuckDBPyConnection> default_connection = nullptr;

static DuckDBPyConnection *DefaultConnection() {
	if (!default_connection) {
		default_connection = DuckDBPyConnection::Connect(":memory:", false);
	}
	return default_connection.get();
}

struct DuckDBPyRelation {

	explicit DuckDBPyRelation(shared_ptr<Relation> rel) : rel(std::move(rel)) {
	}

	static unique_ptr<DuckDBPyRelation> FromDf(py::object df) {
		return DefaultConnection()->FromDF(std::move(df));
	}

	static unique_ptr<DuckDBPyRelation> Values(py::object values = py::list()) {
		return DefaultConnection()->Values(std::move(values));
	}

	static unique_ptr<DuckDBPyRelation> FromCsvAuto(const string &filename) {
		return DefaultConnection()->FromCsvAuto(filename);
	}

	static unique_ptr<DuckDBPyRelation> FromParquet(const string &filename) {
		return DefaultConnection()->FromParquet(filename);
	}

	static unique_ptr<DuckDBPyRelation> FromArrowTable(const py::object &table) {
		return DefaultConnection()->FromArrowTable(table);
	}

	unique_ptr<DuckDBPyRelation> Project(const string &expr) {
		return make_unique<DuckDBPyRelation>(rel->Project(expr));
	}

	static unique_ptr<DuckDBPyRelation> ProjectDf(py::object df, const string &expr) {
		return DefaultConnection()->FromDF(std::move(df))->Project(expr);
	}

	unique_ptr<DuckDBPyRelation> Alias(const string &expr) {
		return make_unique<DuckDBPyRelation>(rel->Alias(expr));
	}

	static unique_ptr<DuckDBPyRelation> AliasDF(py::object df, const string &expr) {
		return DefaultConnection()->FromDF(std::move(df))->Alias(expr);
	}

	unique_ptr<DuckDBPyRelation> Filter(const string &expr) {
		return make_unique<DuckDBPyRelation>(rel->Filter(expr));
	}

	static unique_ptr<DuckDBPyRelation> FilterDf(py::object df, const string &expr) {
		return DefaultConnection()->FromDF(std::move(df))->Filter(expr);
	}

	unique_ptr<DuckDBPyRelation> Limit(int64_t n) {
		return make_unique<DuckDBPyRelation>(rel->Limit(n));
	}

	static unique_ptr<DuckDBPyRelation> LimitDF(py::object df, int64_t n) {
		return DefaultConnection()->FromDF(std::move(df))->Limit(n);
	}

	unique_ptr<DuckDBPyRelation> Order(const string &expr) {
		return make_unique<DuckDBPyRelation>(rel->Order(expr));
	}

	static unique_ptr<DuckDBPyRelation> OrderDf(py::object df, const string &expr) {
		return DefaultConnection()->FromDF(std::move(df))->Order(expr);
	}

	unique_ptr<DuckDBPyRelation> Aggregate(const string &expr, const string &groups = "") {
		if (!groups.empty()) {
			return make_unique<DuckDBPyRelation>(rel->Aggregate(expr, groups));
		}
		return make_unique<DuckDBPyRelation>(rel->Aggregate(expr));
	}

	static unique_ptr<DuckDBPyRelation> AggregateDF(py::object df, const string &expr, const string &groups = "") {
		return DefaultConnection()->FromDF(std::move(df))->Aggregate(expr, groups);
	}

	unique_ptr<DuckDBPyRelation> Distinct() {
		return make_unique<DuckDBPyRelation>(rel->Distinct());
	}

	static unique_ptr<DuckDBPyRelation> DistinctDF(py::object df) {
		return DefaultConnection()->FromDF(std::move(df))->Distinct();
	}

	py::object ToDF() {
		auto res = make_unique<DuckDBPyResult>();
		{
			py::gil_scoped_release release;
			res->result = rel->Execute();
		}
		if (!res->result->success) {
			throw std::runtime_error(res->result->error);
		}
		return res->FetchDF();
	}

	py::object ToArrowTable() {
		auto res = make_unique<DuckDBPyResult>();
		{
			py::gil_scoped_release release;
			res->result = rel->Execute();
		}
		if (!res->result->success) {
			throw std::runtime_error(res->result->error);
		}
		return res->FetchArrowTable();
	}

	unique_ptr<DuckDBPyRelation> Union(DuckDBPyRelation *other) {
		return make_unique<DuckDBPyRelation>(rel->Union(other->rel));
	}

	unique_ptr<DuckDBPyRelation> Except(DuckDBPyRelation *other) {
		return make_unique<DuckDBPyRelation>(rel->Except(other->rel));
	}

	unique_ptr<DuckDBPyRelation> Intersect(DuckDBPyRelation *other) {
		return make_unique<DuckDBPyRelation>(rel->Intersect(other->rel));
	}

	unique_ptr<DuckDBPyRelation> Join(DuckDBPyRelation *other, const string &condition) {
		return make_unique<DuckDBPyRelation>(rel->Join(other->rel, condition));
	}

	void WriteCsv(const string &file) {
		rel->WriteCSV(file);
	}

	static void WriteCsvDF(py::object df, const string &file) {
		return DefaultConnection()->FromDF(std::move(df))->WriteCsv(file);
	}

	// should this return a rel with the new view?
	unique_ptr<DuckDBPyRelation> CreateView(const string &view_name, bool replace = true) {
		rel->CreateView(view_name, replace);
		return make_unique<DuckDBPyRelation>(rel);
	}

	static unique_ptr<DuckDBPyRelation> CreateViewDf(py::object df, const string &view_name, bool replace = true) {
		return DefaultConnection()->FromDF(std::move(df))->CreateView(view_name, replace);
	}

	unique_ptr<DuckDBPyResult> Query(const string &view_name, const string &sql_query) {
		auto res = make_unique<DuckDBPyResult>();
		res->result = rel->Query(view_name, sql_query);
		if (!res->result->success) {
			throw std::runtime_error(res->result->error);
		}
		return res;
	}

	unique_ptr<DuckDBPyResult> Execute() {
		auto res = make_unique<DuckDBPyResult>();
		{
			py::gil_scoped_release release;
			res->result = rel->Execute();
		}
		if (!res->result->success) {
			throw std::runtime_error(res->result->error);
		}
		return res;
	}

	static unique_ptr<DuckDBPyResult> QueryDF(py::object df, const string &view_name, const string &sql_query) {
		return DefaultConnection()->FromDF(std::move(df))->Query(view_name, sql_query);
	}

	void InsertInto(const string &table) {
		rel->Insert(table);
	}

	void Insert(py::object params = py::list()) {
		vector<vector<Value>> values {DuckDBPyConnection::TransformPythonParamList(std::move(params))};
		rel->Insert(values);
	}

	void Create(const string &table) {
		rel->Create(table);
	}

	string Print() {
		std::string rel_res_string;
		{
			py::gil_scoped_release release;
			rel_res_string = rel->Limit(10)->Execute()->ToString();
		}

		return rel->ToString() + "\n---------------------\n-- Result Preview  --\n---------------------\n" +
		       rel_res_string + "\n";
	}

	py::object Getattr(const py::str &key) {
		auto key_s = key.cast<string>();
		if (key_s == "alias") {
			return py::str(string(rel->GetAlias()));
		} else if (key_s == "type") {
			return py::str(RelationTypeToString(rel->type));
		} else if (key_s == "columns") {
			py::list res;
			for (auto &col : rel->Columns()) {
				res.append(col.name);
			}
			return move(res);
		} else if (key_s == "types" || key_s == "dtypes") {
			py::list res;
			for (auto &col : rel->Columns()) {
				res.append(col.type.ToString());
			}
			return move(res);
		}
		return py::none();
	}

	shared_ptr<Relation> rel;
};

enum PySQLTokenType {
	PY_SQL_TOKEN_IDENTIFIER = 0,
	PY_SQL_TOKEN_NUMERIC_CONSTANT,
	PY_SQL_TOKEN_STRING_CONSTANT,
	PY_SQL_TOKEN_OPERATOR,
	PY_SQL_TOKEN_KEYWORD,
	PY_SQL_TOKEN_COMMENT
};

static py::object PyTokenize(const string &query) {
	auto tokens = Parser::Tokenize(query);
	py::list result;
	for (auto &token : tokens) {
		auto tuple = py::tuple(2);
		tuple[0] = token.start;
		switch (token.type) {
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER:
			tuple[1] = PY_SQL_TOKEN_IDENTIFIER;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_NUMERIC_CONSTANT:
			tuple[1] = PY_SQL_TOKEN_NUMERIC_CONSTANT;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT:
			tuple[1] = PY_SQL_TOKEN_STRING_CONSTANT;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR:
			tuple[1] = PY_SQL_TOKEN_OPERATOR;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD:
			tuple[1] = PY_SQL_TOKEN_KEYWORD;
			break;
		case SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT:
			tuple[1] = PY_SQL_TOKEN_COMMENT;
			break;
		}
		result.append(tuple);
	}
	return move(result);
}

PYBIND11_MODULE(duckdb, m) {
	m.doc() = "DuckDB is an embeddable SQL OLAP Database Management System";
	m.attr("__package__") = "duckdb";
	m.attr("__version__") = DuckDB::LibraryVersion();
	m.attr("__git_revision__") = DuckDB::SourceID();

	m.def("connect", &DuckDBPyConnection::Connect,
	      "Create a DuckDB database instance. Can take a database file name to read/write persistent data and a "
	      "read_only flag if no changes are desired",
	      py::arg("database") = ":memory:", py::arg("read_only") = false);
	m.def("tokenize", PyTokenize,
	      "Tokenizes a SQL string, returning a list of (position, type) tuples that can be "
	      "used for e.g. syntax highlighting",
	      py::arg("query"));
	py::enum_<PySQLTokenType>(m, "token_type")
	    .value("identifier", PySQLTokenType::PY_SQL_TOKEN_IDENTIFIER)
	    .value("numeric_const", PySQLTokenType::PY_SQL_TOKEN_NUMERIC_CONSTANT)
	    .value("string_const", PySQLTokenType::PY_SQL_TOKEN_STRING_CONSTANT)
	    .value("operator", PySQLTokenType::PY_SQL_TOKEN_OPERATOR)
	    .value("keyword", PySQLTokenType::PY_SQL_TOKEN_KEYWORD)
	    .value("comment", PySQLTokenType::PY_SQL_TOKEN_COMMENT)
	    .export_values();

	auto conn_class =
	    py::class_<DuckDBPyConnection, shared_ptr<DuckDBPyConnection>>(m, "DuckDBPyConnection")
	        .def("cursor", &DuckDBPyConnection::Cursor, "Create a duplicate of the current connection")
	        .def("duplicate", &DuckDBPyConnection::Cursor, "Create a duplicate of the current connection")
	        .def("execute", &DuckDBPyConnection::Execute,
	             "Execute the given SQL query, optionally using prepared statements with parameters set",
	             py::arg("query"), py::arg("parameters") = py::list(), py::arg("multiple_parameter_sets") = false)
	        .def("executemany", &DuckDBPyConnection::ExecuteMany,
	             "Execute the given prepared statement multiple times using the list of parameter sets in parameters",
	             py::arg("query"), py::arg("parameters") = py::list())
	        .def("close", &DuckDBPyConnection::Close, "Close the connection")
	        .def("fetchone", &DuckDBPyConnection::FetchOne, "Fetch a single row from a result following execute")
	        .def("fetchall", &DuckDBPyConnection::FetchAll, "Fetch all rows from a result following execute")
	        .def("fetchnumpy", &DuckDBPyConnection::FetchNumpy,
	             "Fetch a result as list of NumPy arrays following execute")
	        .def("fetchdf", &DuckDBPyConnection::FetchDF, "Fetch a result as Data.Frame following execute()")
	        .def("fetch_df", &DuckDBPyConnection::FetchDF, "Fetch a result as Data.Frame following execute()")
	        .def("fetch_df_chunk", &DuckDBPyConnection::FetchDFChunk,
	             "Fetch a chunk of the result as Data.Frame following execute()")
	        .def("df", &DuckDBPyConnection::FetchDF, "Fetch a result as Data.Frame following execute()")
	        .def("fetch_arrow_table", &DuckDBPyConnection::FetchArrow,
	             "Fetch a result as Arrow table following execute()")
	        .def("arrow", &DuckDBPyConnection::FetchArrow, "Fetch a result as Arrow table following execute()")
	        .def("begin", &DuckDBPyConnection::Begin, "Start a new transaction")
	        .def("commit", &DuckDBPyConnection::Commit, "Commit changes performed within a transaction")
	        .def("rollback", &DuckDBPyConnection::Rollback, "Roll back changes performed within a transaction")
	        .def("append", &DuckDBPyConnection::Append, "Append the passed Data.Frame to the named table",
	             py::arg("table_name"), py::arg("df"))
	        .def("register", &DuckDBPyConnection::RegisterDF,
	             "Register the passed Data.Frame value for querying with a view", py::arg("view_name"), py::arg("df"))
	        .def("unregister", &DuckDBPyConnection::UnregisterDF, "Unregister the view name", py::arg("view_name"))
	        .def("table", &DuckDBPyConnection::Table, "Create a relation object for the name'd table",
	             py::arg("table_name"))
	        .def("view", &DuckDBPyConnection::View, "Create a relation object for the name'd view",
	             py::arg("view_name"))
	        .def("values", &DuckDBPyConnection::Values, "Create a relation object from the passed values",
	             py::arg("values"))
	        .def("table_function", &DuckDBPyConnection::TableFunction,
	             "Create a relation object from the name'd table function with given parameters", py::arg("name"),
	             py::arg("parameters") = py::list())
	        .def("from_df", &DuckDBPyConnection::FromDF, "Create a relation object from the Data.Frame in df",
	             py::arg("df"))
	        .def("from_arrow_table", &DuckDBPyConnection::FromArrowTable,
	             "Create a relation object from an Arrow table", py::arg("table"))
	        .def("df", &DuckDBPyConnection::FromDF,
	             "Create a relation object from the Data.Frame in df (alias of from_df)", py::arg("df"))
	        .def("from_csv_auto", &DuckDBPyConnection::FromCsvAuto,
	             "Create a relation object from the CSV file in file_name", py::arg("file_name"))
	        .def("from_parquet", &DuckDBPyConnection::FromParquet,
	             "Create a relation object from the Parquet file in file_name", py::arg("file_name"))
	        .def("__getattr__", &DuckDBPyConnection::GetAttr, "Get result set attributes, mainly column names");

	py::class_<DuckDBPyResult>(m, "DuckDBPyResult")
	    .def("close", &DuckDBPyResult::Close)
	    .def("fetchone", &DuckDBPyResult::Fetchone)
	    .def("fetchall", &DuckDBPyResult::Fetchall)
	    .def("fetchnumpy", &DuckDBPyResult::FetchNumpy)
	    .def("fetchdf", &DuckDBPyResult::FetchDF)
	    .def("fetch_df", &DuckDBPyResult::FetchDF)
	    .def("fetch_df_chunk", &DuckDBPyResult::FetchDFChunk)
	    .def("fetch_arrow_table", &DuckDBPyResult::FetchArrowTable)
	    .def("arrow", &DuckDBPyResult::FetchArrowTable)
	    .def("df", &DuckDBPyResult::FetchDF);

	py::class_<DuckDBPyRelation>(m, "DuckDBPyRelation")
	    .def("filter", &DuckDBPyRelation::Filter, "Filter the relation object by the filter in filter_expr",
	         py::arg("filter_expr"))
	    .def("project", &DuckDBPyRelation::Project, "Project the relation object by the projection in project_expr",
	         py::arg("project_expr"))
	    .def("set_alias", &DuckDBPyRelation::Alias, "Rename the relation object to new alias", py::arg("alias"))
	    .def("order", &DuckDBPyRelation::Order, "Reorder the relation object by order_expr", py::arg("order_expr"))
	    .def("aggregate", &DuckDBPyRelation::Aggregate,
	         "Compute the aggregate aggr_expr by the optional groups group_expr on the relation", py::arg("aggr_expr"),
	         py::arg("group_expr") = "")
	    .def("union", &DuckDBPyRelation::Union,
	         "Create the set union of this relation object with another relation object in other_rel")
	    .def("except_", &DuckDBPyRelation::Except,
	         "Create the set except of this relation object with another relation object in other_rel",
	         py::arg("other_rel"))
	    .def("intersect", &DuckDBPyRelation::Intersect,
	         "Create the set intersection of this relation object with another relation object in other_rel",
	         py::arg("other_rel"))
	    .def("join", &DuckDBPyRelation::Join,
	         "Join the relation object with another relation object in other_rel using the join condition expression "
	         "in join_condition",
	         py::arg("other_rel"), py::arg("join_condition"))
	    .def("distinct", &DuckDBPyRelation::Distinct, "Retrieve distinct rows from this relation object")
	    .def("limit", &DuckDBPyRelation::Limit, "Only retrieve the first n rows from this relation object",
	         py::arg("n"))
	    .def("query", &DuckDBPyRelation::Query,
	         "Run the given SQL query in sql_query on the view named virtual_table_name that refers to the relation "
	         "object",
	         py::arg("virtual_table_name"), py::arg("sql_query"))
	    .def("execute", &DuckDBPyRelation::Execute, "Transform the relation into a result set")
	    .def("write_csv", &DuckDBPyRelation::WriteCsv, "Write the relation object to a CSV file in file_name",
	         py::arg("file_name"))
	    .def("insert_into", &DuckDBPyRelation::InsertInto,
	         "Inserts the relation object into an existing table named table_name", py::arg("table_name"))
	    .def("insert", &DuckDBPyRelation::Insert, "Inserts the given values into the relation", py::arg("values"))
	    .def("create", &DuckDBPyRelation::Create,
	         "Creates a new table named table_name with the contents of the relation object", py::arg("table_name"))
	    .def("create_view", &DuckDBPyRelation::CreateView,
	         "Creates a view named view_name that refers to the relation object", py::arg("view_name"),
	         py::arg("replace") = true)
	    .def("to_arrow_table", &DuckDBPyRelation::ToArrowTable, "Transforms the relation object into a Arrow table")
	    .def("arrow", &DuckDBPyRelation::ToArrowTable, "Transforms the relation object into a Arrow table")
	    .def("to_df", &DuckDBPyRelation::ToDF, "Transforms the relation object into a Data.Frame")
	    .def("df", &DuckDBPyRelation::ToDF, "Transforms the relation object into a Data.Frame")
	    .def("__str__", &DuckDBPyRelation::Print)
	    .def("__repr__", &DuckDBPyRelation::Print)
	    .def("__getattr__", &DuckDBPyRelation::Getattr);

	m.def("values", &DuckDBPyRelation::Values, "Create a relation object from the passed values", py::arg("values"));
	m.def("from_csv_auto", &DuckDBPyRelation::FromCsvAuto, "Creates a relation object from the CSV file in file_name",
	      py::arg("file_name"));
	m.def("from_parquet", &DuckDBPyRelation::FromParquet,
	      "Creates a relation object from the Parquet file in file_name", py::arg("file_name"));
	m.def("df", &DuckDBPyRelation::FromDf, "Create a relation object from the Data.Frame df", py::arg("df"));
	m.def("from_df", &DuckDBPyRelation::FromDf, "Create a relation object from the Data.Frame df", py::arg("df"));
	m.def("from_arrow_table", &DuckDBPyRelation::FromArrowTable, "Create a relation object from an Arrow table",
	      py::arg("table"));
	m.def("arrow", &DuckDBPyRelation::FromArrowTable, "Create a relation object from an Arrow table", py::arg("table"));
	m.def("filter", &DuckDBPyRelation::FilterDf, "Filter the Data.Frame df by the filter in filter_expr", py::arg("df"),
	      py::arg("filter_expr"));
	m.def("project", &DuckDBPyRelation::ProjectDf, "Project the Data.Frame df by the projection in project_expr",
	      py::arg("df"), py::arg("project_expr"));
	m.def("alias", &DuckDBPyRelation::AliasDF, "Create a relation from Data.Frame df with the passed alias",
	      py::arg("df"), py::arg("alias"));
	m.def("order", &DuckDBPyRelation::OrderDf, "Reorder the Data.Frame df by order_expr", py::arg("df"),
	      py::arg("order_expr"));
	m.def("aggregate", &DuckDBPyRelation::AggregateDF,
	      "Compute the aggregate aggr_expr by the optional groups group_expr on Data.frame df", py::arg("df"),
	      py::arg("aggr_expr"), py::arg("group_expr") = "");
	m.def("distinct", &DuckDBPyRelation::DistinctDF, "Compute the distinct rows from Data.Frame df ", py::arg("df"));
	m.def("limit", &DuckDBPyRelation::LimitDF, "Retrieve the first n rows from the Data.Frame df", py::arg("df"),
	      py::arg("n"));
	m.def("query", &DuckDBPyRelation::QueryDF,
	      "Run the given SQL query in sql_query on the view named virtual_table_name that contains the content of "
	      "Data.Frame df",
	      py::arg("df"), py::arg("virtual_table_name"), py::arg("sql_query"));
	m.def("write_csv", &DuckDBPyRelation::WriteCsvDF, "Write the Data.Frame df to a CSV file in file_name",
	      py::arg("df"), py::arg("file_name"));

	// we need this because otherwise we try to remove registered_dfs on shutdown when python is already dead
	auto clean_default_connection = []() {
		default_connection.reset();
	};
	m.add_object("_clean_default_connection", py::capsule(clean_default_connection));
	PyDateTime_IMPORT;
}

} // namespace duckdb
