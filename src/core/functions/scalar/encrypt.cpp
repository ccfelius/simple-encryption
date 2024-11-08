#define DUCKDB_EXTENSION_MAIN

#define TEST_KEY "0123456789112345"

// what is the maximum size of biggest type in duckdb
#define MAX_BUFFER_SIZE 1024

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "mbedtls_wrapper.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "simple_encryption/core/functions/scalar/encrypt.hpp"
#include "simple_encryption/core/functions/scalar.hpp"
#include "simple_encryption_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "simple_encryption/core/functions/function_data/encrypt_function_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace simple_encryption {

namespace core {

SimpleEncryptKeys &SimpleEncryptKeys::Get(ClientContext &context) {
  auto &cache = ObjectCache::GetObjectCache(context);
  if (!cache.Get<SimpleEncryptKeys>(SimpleEncryptKeys::ObjectType())) {
    cache.Put(SimpleEncryptKeys::ObjectType(), make_shared_ptr<SimpleEncryptKeys>());
  }
  return *cache.Get<SimpleEncryptKeys>(SimpleEncryptKeys::ObjectType());
}

void SimpleEncryptKeys::AddKey(const string &key_name, const string &key) {
  keys[key_name] = key;
}

bool SimpleEncryptKeys::HasKey(const string &key_name) const {
  return keys.find(key_name) != keys.end();
}

const string &SimpleEncryptKeys::GetKey(const string &key_name) const {
  D_ASSERT(HasKey(key_name));
  return keys.at(key_name);
}

string SimpleEncryptKeys::ObjectType() {
  return "simple_encrypt_keys";
}

string SimpleEncryptKeys::GetObjectType() {
  return ObjectType();
}

shared_ptr<EncryptionUtil> GetEncryptionUtil(ExpressionState &state) {
  auto &func_expr = (BoundFunctionExpression &)state.expr;
  auto &info = (EncryptFunctionData &)*func_expr.bind_info;
  // get Database config
  auto &config = DBConfig::GetConfig(*info.context.db);
  return config.encryption_util;
}

shared_ptr<EncryptionState> InitializeCryptoState(ExpressionState &state) {
  auto encryption_state = GetEncryptionUtil(state);

  if (!encryption_state) {
    return duckdb_mbedtls::MbedTlsWrapper::AESGCMStateMBEDTLSFactory()
                   .CreateEncryptionState();
  }

  return encryption_state->CreateEncryptionState();
}

shared_ptr<EncryptionState> InitializeDecryption(ExpressionState &state) {

  // For now, hardcode everything
  const string key = TEST_KEY;
  unsigned char iv[16];
  memcpy((void *)iv, "12345678901", 16);
  //
  //  // TODO; construct nonce based on immutable ROW_ID + hash(col_name)
  iv[12] = 0x00;
  iv[13] = 0x00;
  iv[14] = 0x00;
  iv[15] = 0x00;

  auto decryption_state = InitializeCryptoState(state);
  decryption_state->InitializeDecryption(iv, 16, &key);

  return decryption_state;
}

inline const uint8_t *DecryptValue(uint8_t *buffer, size_t size, ExpressionState &state) {

  // Initialize Encryption
  auto encryption_state = InitializeDecryption(state);
  uint8_t decryption_buffer[MAX_BUFFER_SIZE];
  uint8_t *temp_buf = decryption_buffer;

  encryption_state->Process(buffer, size, temp_buf, size);

  return temp_buf;
}

bool CheckEncryption(string_t printable_encrypted_data, uint8_t *buffer,
                            size_t size, const uint8_t *value, ExpressionState &state){

  // cast encrypted data to blob back and forth
  // to check whether data will be lost with casting
  auto unblobbed_data = Blob::ToBlob(printable_encrypted_data);
  auto encrypted_unblobbed_data =
      reinterpret_cast<const uint8_t *>(unblobbed_data.data());

  if (memcmp(encrypted_unblobbed_data, buffer, size) != 0) {
    throw InvalidInputException(
        "Original Encrypted Data differs from Unblobbed Encrypted Data");
  }

  auto decrypted_data = DecryptValue(buffer, size, state);
  if (memcmp(decrypted_data, value, size) != 0) {
    throw InvalidInputException(
        "Original Data differs from Decrypted Data");
  }
  return true;
}


template <typename T>
typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value, T>::type
ConvertToCipherText(Vector &vector, uint8_t *buffer_p, size_t data_size) {
  T encrypted_data;
  memcpy(&encrypted_data, buffer_p, sizeof(T));
  return encrypted_data;
}

// Handle string_t type and convert to Base64
template <typename T>
typename std::enable_if<std::is_same<T, string_t>::value, T>::type
ConvertToCipherText(Vector &vector, uint8_t *buffer_p, size_t data_size) {

  string_t input(reinterpret_cast<const char *>(buffer_p), data_size);
  size_t base64_size = Blob::ToBase64Size(input);
  string_t output = StringVector::EmptyString(vector, base64_size);

  // Convert blob to base64
  Blob::ToBase64(input, output.GetDataWriteable());

  return output;
}

// Template specialization for integral and floating point types
// in decrypt, we want to translate the encrypted values to a buffer
template <typename T>
typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value, const char*>::type
ConvertFromCipherText(uint8_t *buffer_p, size_t data_size, T *input_data) {
  T decrypted_data;
  memcpy(&decrypted_data, buffer_p, sizeof(T));
  return decrypted_data;
}

// Template specialization for string_t
template <typename T>
typename std::enable_if<std::is_same<T, string_t>::value, const char*>::type
ConvertFromCipherText(uint8_t *buffer_p, size_t data_size, T *input_data) {

  // Decode Base64-encoded input into binary format
  string_t input(reinterpret_cast<const char *>(buffer_p), data_size);
  size_t base64_size = Blob::FromBase64Size(input);

  // Convert from base64 to blob, storing it back in buffer_p
  Blob::FromBase64(input, buffer_p, base64_size);

  // return the buffer just directly
  return input.GetString();
}

//template <typename T>
//typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value, T>::type
//ConvertFromCipherText(uint8_t *buffer_p, size_t data_size, const uint8_t *input_data) {
//  T decrypted_data;
//  memcpy(&decrypted_data, buffer_p, sizeof(T));
//  return decrypted_data;
//}
//
//template <typename T>
//typename std::enable_if<std::is_same<T, string_t>::value, uint8_t *>::type
//ConvertFromCipherText(uint8_t *buffer_p, size_t data_size, const uint8_t *input_data) {
//
//  string_t input(reinterpret_cast<const char *>(buffer_p), data_size);
//  size_t base64_size = Blob::FromBase64Size(input);
//
//  // Convert From base64 to blob
//  Blob::FromBase64(input, buffer_p, base64_size);
//
//  return buffer_p;
//}

// TODO: for decryption, convert a string to blob and then decrypt and then return string_t?
//template <typename T>
//typename std::enable_if<std::is_same<T, string_t>::value, T>::type
//ConvertToCipherText(uint8_t *buffer_p, size_t data_size, const uint8_t *input_data) {
//  //
//  return string_t(reinterpret_cast<const char *>(buffer_p), data_size);
//}

// Catch-all for unsupported types
template <typename T>
typename std::enable_if<!std::is_integral<T>::value && !std::is_floating_point<T>::value && !std::is_same<T, string_t>::value, T>::type
ConvertToCipherText(uint8_t *buffer_p, size_t data_size, const uint8_t *input_data) {
  throw std::invalid_argument("Unsupported type for Encryption");
}

template <typename T>
typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value, T>::type
GetSizeOfInput(const T &input) {
  // For numeric types, use sizeof(T) directly
  return sizeof(T);
}

// Specialized template for string_t type
template <typename T>
typename std::enable_if<std::is_same<T, string_t>::value, size_t>::type
GetSizeOfInput(const T &input) {
  // For string_t, get actual string data size
  return input.GetSize();
}

// General template for numeric types
template <typename T>
typename std::enable_if<std::is_integral<T>::value || std::is_floating_point<T>::value, const char*>::type
GetCharData(const T &input) {
  return reinterpret_cast<const char*>(&input);
}

// Specialized template for string_t type
template <typename T>
typename std::enable_if<std::is_same<T, string_t>::value, const char*>::type
GetCharData(const T &input) {
  return input.GetData();
}
//---------------------------------------------------------------------------------------------


template <typename T>
void ExecuteEncryptExecutor(Vector &vector, Vector &result, idx_t size, ExpressionState &state, const string &key_t) {

  // TODO: put this in the state of the extension
  uint8_t encryption_buffer[MAX_BUFFER_SIZE];
  uint8_t *buffer_p = encryption_buffer;

  unsigned char iv[16];
  auto encryption_state = InitializeCryptoState(state);

  // TODO: construct nonce based on immutable ROW_ID + hash(col_name)
  memcpy(iv, "12345678901", 12);
  iv[12] = iv[13] = iv[14] = iv[15] = 0x00;

  UnaryExecutor::Execute<T, T>(vector, result, size, [&](T input) -> T {

//    unsigned char byte_array[sizeof(T)];
    auto data_size = GetSizeOfInput(input);
    encryption_state->InitializeEncryption(iv, 16, &key_t);

//    data_ptr_t val = static_cast<unsigned char*>(&input);
    // Convert input to byte array for processing
//    memcpy(byte_array, &input, sizeof(T));
//
//    auto input_data = data_ptr_t(GetCharData(input));

    // Encrypt data
    encryption_state->Process(data_ptr_t(GetCharData(input)), data_size, buffer_p, data_size);
    T encrypted_data = ConvertToCipherText<T>(result, buffer_p, data_size);

#if 0
        D_ASSERT(CheckEncryption(printable_encrypted_data, buffer_p, size, reinterpret_cast<const_data_ptr_t>(name.GetData()), state) == 1);
#endif

    // this does not do anything for CTR and therefore can be skipped
    encryption_state->Finalize(buffer_p, 0, nullptr, 0);
    return encrypted_data;
  });
}

// Generated code
//---------------------------------------------------------------------------------------------

// Helper function that dispatches the runtime type to the appropriate templated function
void ExecuteEncrypt(Vector &vector, Vector &result, idx_t size, ExpressionState &state, const string &key_t) {
  // Check the vector type and call the correct templated version
  switch (vector.GetType().id()) {
  case LogicalTypeId::INTEGER:
    ExecuteEncryptExecutor<int32_t>(vector, result, size, state, key_t);
    break;
  case LogicalTypeId::BIGINT:
    ExecuteEncryptExecutor<int64_t>(vector, result, size, state, key_t);
    break;
  case LogicalTypeId::VARCHAR:
    ExecuteEncryptExecutor<string_t>(vector, result, size, state, key_t);
    break;
  default:
    throw NotImplementedException("Unsupported type for Encryption");
  }
}
//---------------------------------------------------------------------------------------------

template <typename T>
void ExecuteDecryptExecutor(Vector &vector, Vector &result, idx_t size, ExpressionState &state, const string &key_t) {

  // TODO: put this in the state of the extension
  uint8_t encryption_buffer[MAX_BUFFER_SIZE];
  uint8_t *buffer_p = encryption_buffer;

  unsigned char iv[16];
  auto encryption_state = InitializeCryptoState(state);

  // TODO: construct nonce based on immutable ROW_ID + hash(col_name)
  memcpy(iv, "12345678901", 12);
  iv[12] = iv[13] = iv[14] = iv[15] = 0x00;

  UnaryExecutor::Execute<T, T>(vector, result, size, [&](T input) -> T {
    // TODO: IMPROVE THIS!!!!
    // this can also be placed more upper
    encryption_state->InitializeDecryption(iv, 16, &key_t);
    auto data_size = GetSizeOfInput(input);
    const_data_ptr_t input_data = reinterpret_cast<const_data_ptr_t>(GetCharData(input));

//    // Convert input to byte array for processing
//    memcpy(buffer_p, &input, sizeof(T));

    // Decrypt data
    encryption_state->Process(input_data, data_size, buffer_p, data_size);

    // we don't need to give an input
    T decrypted_data = ConvertToCipherText<T>(vector, buffer_p, data_size);

#if 0
        D_ASSERT(CheckEncryption(printable_encrypted_data, buffer_p, size, reinterpret_cast<const_data_ptr_t>(name.GetData()), state) == 1);
#endif


    // this does not do anything for CTR and therefore can be skipped
    encryption_state->Finalize(buffer_p, 0, nullptr, 0);
    return decrypted_data;
  });
}

// Generated code
//---------------------------------------------------------------------------------------------

// Helper function that dispatches the runtime type to the appropriate templated function
void ExecuteDecrypt(Vector &vector, Vector &result, idx_t size, ExpressionState &state, const string &key_t) {
  // Check the vector type and call the correct templated version
  switch (vector.GetType().id()) {
  case LogicalTypeId::INTEGER:
    ExecuteDecryptExecutor<int32_t>(vector, result, size, state, key_t);
    break;
  case LogicalTypeId::BIGINT:
    ExecuteDecryptExecutor<int64_t>(vector, result, size, state, key_t);
    break;
  case LogicalTypeId::VARCHAR:
    ExecuteDecryptExecutor<string_t>(vector, result, size, state, key_t);
    break;
  default:
    throw NotImplementedException("Unsupported type for Encryption");
  }
}
//---------------------------------------------------------------------------------------------

static void EncryptData(DataChunk &args, ExpressionState &state, Vector &result) {

  auto &value_vector = args.data[0];

  // Get the encryption key
  auto &key_vector = args.data[1];
  D_ASSERT(key_vector.GetVectorType() == VectorType::CONSTANT_VECTOR);

  // Fetch the encryption key as a constant string
  const string key_t = ConstantVector::GetData<string_t>(key_vector)[0].GetString();

  // can we not pass by reference?
  ExecuteEncrypt(value_vector, result, args.size(), state, key_t);
}

static void DecryptData(DataChunk &args, ExpressionState &state, Vector &result) {

  auto &value_vector = args.data[0];

  // Get the encryption key
  auto &key_vector = args.data[1];
  D_ASSERT(key_vector.GetVectorType() == VectorType::CONSTANT_VECTOR);

  // Fetch the encryption key as a constant string
  const string key_t = ConstantVector::GetData<string_t>(key_vector)[0].GetString();

  // can we not pass by reference?
  ExecuteDecrypt(value_vector, result, args.size(), state, key_t);
}

ScalarFunctionSet GetEncryptionFunction() {
  ScalarFunctionSet set("encrypt");

  set.AddFunction(ScalarFunction({LogicalTypeId::INTEGER, LogicalType::VARCHAR}, LogicalTypeId::INTEGER, EncryptData,
                                 EncryptFunctionData::EncryptBind));

  set.AddFunction(ScalarFunction({LogicalTypeId::BIGINT, LogicalType::VARCHAR}, LogicalTypeId::BIGINT, EncryptData,
                                 EncryptFunctionData::EncryptBind));

//  set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BLOB, EncryptData,
//                                 EncryptFunctionData::EncryptBind));

  set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, EncryptData,
                                 EncryptFunctionData::EncryptBind));

  return set;
}

ScalarFunctionSet GetDecryptionFunction() {
  ScalarFunctionSet set("decrypt");

  // input is column of any type, key is of type VARCHAR, output is of same type
  set.AddFunction(ScalarFunction({LogicalTypeId::INTEGER, LogicalType::VARCHAR}, LogicalTypeId::INTEGER, DecryptData,
                                 EncryptFunctionData::EncryptBind));

  set.AddFunction(ScalarFunction({LogicalTypeId::BIGINT, LogicalType::VARCHAR}, LogicalTypeId::BIGINT, DecryptData,
                                 EncryptFunctionData::EncryptBind));

  set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, DecryptData,
                                 EncryptFunctionData::EncryptBind));

//  set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BLOB, DecryptData,
//                                 EncryptFunctionData::EncryptBind));

  return set;
}

//------------------------------------------------------------------------------
// Register functions
//------------------------------------------------------------------------------

void CoreScalarFunctions::RegisterEncryptDataScalarFunction(
    DatabaseInstance &db) {
  ExtensionUtil::RegisterFunction(db, GetEncryptionFunction());
  ExtensionUtil::RegisterFunction(db, GetDecryptionFunction());
}
}
}