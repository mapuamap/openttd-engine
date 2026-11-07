/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file agent_command_catalog.cpp Catalog of every engine command and generic normal-command dispatch. */

#include "stdafx.h"
#include "agent_command_catalog.h"

#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "core/format.hpp"
#include "core/pool_type.hpp"
#include "network/network.h"
#include "goal_type.h"
#include "news_type.h"
#include "order_base.h"
#include "order_type.h"
#include "source_type.h"
#include "strings_func.h"
#include "vehiclelist.h"
#include "vehicle_type.h"

#include "airport_cmd.h"
#include "aircraft_cmd.h"
#include "autoreplace_cmd.h"
#include "blueprint_cmd.h"
#include "rail_route_cmd.h"
#include "company_cmd.h"
#include "depot_cmd.h"
#include "dock_cmd.h"
#include "economy_cmd.h"
#include "engine_cmd.h"
#include "goal_cmd.h"
#include "group_cmd.h"
#include "industry_cmd.h"
#include "landscape_cmd.h"
#include "league_cmd.h"
#include "misc_cmd.h"
#include "news_cmd.h"
#include "object_cmd.h"
#include "order_cmd.h"
#include "rail_cmd.h"
#include "road_cmd.h"
#include "roadveh_cmd.h"
#include "settings_cmd.h"
#include "signs_cmd.h"
#include "station_cmd.h"
#include "story_cmd.h"
#include "subsidy_cmd.h"
#include "terraform_cmd.h"
#include "timetable_cmd.h"
#include "town_cmd.h"
#include "train_cmd.h"
#include "tree_cmd.h"
#include "tunnelbridge_cmd.h"
#include "vehicle_cmd.h"
#include "viewport_cmd.h"
#include "water_cmd.h"
#include "waypoint_cmd.h"

#include "table/strings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** What the engine reported for the command that was posted last. */
struct PostedCommand {
	bool captured = false; ///< The engine reported a result, so the command really ran.
	CommandCost cost; ///< Actual cost, or the failure of the command.
};

/** Capture slot of the command that is currently being posted. */
PostedCommand _posting;

/* -- type inspection --------------------------------------------------------------------- */

/** Recognise a StrongType::Typedef and keep its tag, so the catalog can name it. */
template <typename T>
struct StrongTypedefInfo {
	static constexpr bool is = false; ///< Whether T is a StrongType::Typedef.
};

/** @copydoc StrongTypedefInfo */
template <typename TBase, typename TTag, typename... TProps>
struct StrongTypedefInfo<StrongType::Typedef<TBase, TTag, TProps...>> {
	static constexpr bool is = true; ///< Whether T is a StrongType::Typedef.
	using Base = TBase; ///< The wrapped value type.
	using Tag = TTag; ///< The tag that keeps the typedef distinct.
};

/** Recognise an EnumBitSet and keep its enum, storage and end marker. */
template <typename T>
struct EnumBitSetInfo {
	static constexpr bool is = false; ///< Whether T is an EnumBitSet.
};

/** @copydoc EnumBitSetInfo */
template <typename TEnum, typename TStorage, TEnum TEnd>
struct EnumBitSetInfo<EnumBitSet<TEnum, TStorage, TEnd>> {
	static constexpr bool is = true; ///< Whether T is an EnumBitSet.
	using Enum = TEnum; ///< The enum the bits are taken from.
	using Storage = TStorage; ///< The integer that stores the bits.
	static constexpr TEnum end = TEnd; ///< Last valid value + 1, as far as the enum declares it.
};

/**
 * Recognise an enum whose valid values are contiguous from 0 up to the engine's own end marker.
 *
 * Only enums that declare such a marker are listed: guessing a range from the storage width would
 * accept enumerators the engine never defines.
 */
template <typename T>
struct EnumEndInfo {
	static constexpr bool is = false; ///< Whether the enum declares its end marker.
};

/** @copydoc EnumEndInfo */
template <>
struct EnumEndInfo<VehicleType> {
	static constexpr bool is = true; ///< Whether the enum declares its end marker.
	static constexpr uint64_t end = to_underlying(VehicleType::End); ///< Last valid value + 1.
};

/** @copydoc EnumEndInfo */
template <>
struct EnumEndInfo<NewsType> {
	static constexpr bool is = true; ///< Whether the enum declares its end marker.
	static constexpr uint64_t end = to_underlying(NewsType::End); ///< Last valid value + 1.
};

/** @copydoc EnumEndInfo */
template <>
struct EnumEndInfo<GoalQuestionType> {
	static constexpr bool is = true; ///< Whether the enum declares its end marker.
	static constexpr uint64_t end = to_underlying(GoalQuestionType::End); ///< Last valid value + 1.
};

/** Recognise a pool id and keep its tag, so the catalog can name it. */
template <typename T>
struct PoolIdInfo {
	static constexpr bool is = false; ///< Whether T is a PoolID.
};

/** @copydoc PoolIdInfo */
template <typename TBase, typename TTag, TBase TMax, TBase TInvalid>
struct PoolIdInfo<PoolID<TBase, TTag, TMax, TInvalid>> {
	static constexpr bool is = true; ///< Whether T is a PoolID.
	using Tag = TTag; ///< The tag that keeps the pool id distinct.
};

/** Recognise a std::variant. */
template <typename T>
struct VariantInfo {
	static constexpr bool is = false; ///< Whether T is a std::variant.
};

/** @copydoc VariantInfo */
template <typename... Ts>
struct VariantInfo<std::variant<Ts...>> {
	static constexpr bool is = true; ///< Whether T is a std::variant.
};

/** Recognise a type that wraps a single value it can be constructed from (`BaseType`). */
template <typename T, typename = void>
struct HasBaseType {
	static constexpr bool value = false; ///< Whether T has a `BaseType` it can be built from.
};

/** @copydoc HasBaseType */
template <typename T>
struct HasBaseType<T, std::void_t<typename T::BaseType>> {
	static constexpr bool value = true; ///< Whether T has a `BaseType` it can be built from.
	using Base = typename T::BaseType; ///< The wrapped value type.
};

/** Recognise a std::vector. */
template <typename T>
struct StdVectorInfo {
	static constexpr bool is = false; ///< Whether T is a std::vector.
};

/** @copydoc StdVectorInfo */
template <typename TValue, typename TAlloc>
struct StdVectorInfo<std::vector<TValue, TAlloc>> {
	static constexpr bool is = true; ///< Whether T is a std::vector.
	using Value = TValue; ///< Element type.
};

/** Recognise a std::array. */
template <typename T>
struct StdArrayInfo {
	static constexpr bool is = false; ///< Whether T is a std::array.
};

/** @copydoc StdArrayInfo */
template <typename TValue, size_t TSize>
struct StdArrayInfo<std::array<TValue, TSize>> {
	static constexpr bool is = true; ///< Whether T is a std::array.
	using Value = TValue; ///< Element type.
	static constexpr size_t size = TSize; ///< Number of elements.
};

/** Recognise a std::tuple. */
template <typename T>
struct StdTupleInfo {
	static constexpr bool is = false; ///< Whether T is a std::tuple.
};

/** @copydoc StdTupleInfo */
template <typename... Ts>
struct StdTupleInfo<std::tuple<Ts...>> {
	static constexpr bool is = true; ///< Whether T is a std::tuple.
};

/** Where the entity id of one argument lives; defined with the command targets below. */
template <typename T>
Json TargetSchema();

/** The catalog name of one alternative of a variant argument; defined with the variant decoder. */
template <typename T>
std::string VariantAlternativeName();

/** The catalog name of a variant argument type. */
template <typename... Ts>
std::string VariantTypeName(const std::variant<Ts...> *)
{
	std::string name = "variant<";
	size_t index = 0;
	((name += (index++ == 0 ? "" : "|"), name += VariantAlternativeName<Ts>()), ...);
	return name + ">";
}

/**
 * The name of a type as the compiler spells it.
 * @return The raw type name, empty when the compiler gives no usable spelling.
 */
template <typename T>
constexpr std::string_view RawTypeName()
{
#if defined(__clang__) || defined(__GNUC__)
	const std::string_view signature = __PRETTY_FUNCTION__;
	const std::string_view prefix = "T = ";
	const size_t start = signature.find(prefix);
	if (start == std::string_view::npos) return {};
	const size_t begin = start + prefix.size();
	const size_t end = signature.find_first_of(";]", begin);
	if (end == std::string_view::npos) return {};
	return signature.substr(begin, end - begin);
#elif defined(_MSC_VER)
	const std::string_view signature = __FUNCSIG__;
	const std::string_view prefix = "RawTypeName<";
	const size_t start = signature.find(prefix);
	if (start == std::string_view::npos) return {};
	const size_t begin = start + prefix.size();
	const size_t end = signature.find('>', begin);
	if (end == std::string_view::npos) return {};
	return signature.substr(begin, end - begin);
#else
	return {};
#endif
}

/**
 * Turn a StrongType tag into the name of the type it belongs to: `TileIndexTag` becomes `TileIndex`.
 * @return The type name derived from the tag.
 */
template <typename TTag>
std::string TagTypeName()
{
	std::string_view raw = RawTypeName<TTag>();
	for (const std::string_view prefix : {"struct ", "class ", "enum "}) {
		if (raw.starts_with(prefix)) {
			raw.remove_prefix(prefix.size());
			break;
		}
	}
	if (raw.ends_with("Tag")) raw.remove_suffix(3);
	return std::string(raw);
}

/**
 * The catalog name of a command argument type.
 * @return A stable, readable type name.
 */
template <typename T>
std::string TypeName()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return "bool";
	else if constexpr (std::is_same_v<U, uint8_t>) return "uint8_t";
	else if constexpr (std::is_same_v<U, uint16_t>) return "uint16_t";
	else if constexpr (std::is_same_v<U, uint32_t>) return "uint32_t";
	else if constexpr (std::is_same_v<U, uint64_t>) return "uint64_t";
	else if constexpr (std::is_same_v<U, int8_t>) return "int8_t";
	else if constexpr (std::is_same_v<U, int16_t>) return "int16_t";
	else if constexpr (std::is_same_v<U, int32_t>) return "int32_t";
	else if constexpr (std::is_same_v<U, int64_t>) return "int64_t";
	else if constexpr (std::is_same_v<U, Money>) return "Money";
	else if constexpr (std::is_same_v<U, std::monostate>) return "monostate";
	else if constexpr (std::is_same_v<U, EncodedString>) return "EncodedString";
	else if constexpr (std::is_same_v<U, Source>) return "Source";
	else if constexpr (std::is_same_v<U, VehicleListIdentifier>) return "VehicleListIdentifier";
	else if constexpr (std::is_same_v<U, Order>) return "Order";
	else if constexpr (std::is_same_v<U, BlueprintChunkData>) return "BlueprintChunkData";
	else if constexpr (std::is_same_v<U, std::string>) return "string";
	else if constexpr (std::is_same_v<U, EncodedString>) return "encoded_string";
	else if constexpr (std::is_same_v<U, Order>) return "order";
	else if constexpr (std::is_same_v<U, Source>) return "source";
	else if constexpr (std::is_same_v<U, VehicleListIdentifier>) return "vehicle_list";
	else if constexpr (VariantInfo<U>::is) return VariantTypeName(static_cast<const U *>(nullptr));
	else if constexpr (StrongTypedefInfo<U>::is) return TagTypeName<typename StrongTypedefInfo<U>::Tag>();
	else if constexpr (PoolIdInfo<U>::is) return TagTypeName<typename PoolIdInfo<U>::Tag>();
	else if constexpr (EnumBitSetInfo<U>::is) return "EnumBitSet<" + TypeName<typename EnumBitSetInfo<U>::Enum>() + ">";
	else if constexpr (StdVectorInfo<U>::is) return "vector<" + TypeName<typename StdVectorInfo<U>::Value>() + ">";
	else if constexpr (StdArrayInfo<U>::is) return "array<" + TypeName<typename StdArrayInfo<U>::Value>() + ">";
	else if constexpr (StdTupleInfo<U>::is) return std::string(RawTypeName<U>());
	else if constexpr (HasBaseType<U>::value) return std::string(RawTypeName<U>());
	else return std::string(RawTypeName<U>());
}

/**
 * The catalog kind of a command argument type.
 * @return The kind name: boolean, integer, enum, strong_id, enum_bit_set, string, vector, array,
 *         tuple, or unsupported.
 */
template <typename T>
const char *ArgKind()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return "boolean";
	else if constexpr (std::is_integral_v<U>) return "integer";
	else if constexpr (std::is_enum_v<U>) return "enum";
	else if constexpr (EnumBitSetInfo<U>::is) return "enum_bit_set";
	else if constexpr (std::is_same_v<U, std::string>) return "string";
	else if constexpr (StdVectorInfo<U>::is) return "vector";
	else if constexpr (StdArrayInfo<U>::is) return "array";
	else if constexpr (StdTupleInfo<U>::is) return "tuple";
	else if constexpr (VariantInfo<U>::is) return "variant";
	else if constexpr (HasBaseType<U>::value) return "strong_id";
	else return "unsupported";
}

/* -- what the decoder accepts ------------------------------------------------------------ */

template <typename T>
constexpr bool ComputeDecodable();

/** Whether every alternative of a variant can be decoded. */
template <typename... Ts>
constexpr bool VariantDecodable(const std::variant<Ts...> *)
{
	return (ComputeDecodable<Ts>() && ...);
}

/** Whether every element of a tuple can be decoded. */
template <typename... Ts>
constexpr bool TupleDecodable(const std::tuple<Ts...> *)
{
	return (ComputeDecodable<Ts>() && ...);
}

/**
 * Whether the generic decoder can build a value of type T from JSON.
 * @return True when T is supported.
 */
template <typename T>
constexpr bool ComputeDecodable()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, bool>) return true;
	else if constexpr (std::is_same_v<U, std::monostate>) return true;
	else if constexpr (std::is_integral_v<U>) return true;
	else if constexpr (std::is_enum_v<U>) return true;
	else if constexpr (EnumBitSetInfo<U>::is) return true;
	else if constexpr (std::is_same_v<U, std::string>) return true;
	else if constexpr (std::is_same_v<U, EncodedString>) return true;
	else if constexpr (std::is_same_v<U, Source>) return true;
	else if constexpr (std::is_same_v<U, VehicleListIdentifier>) return true;
	else if constexpr (std::is_same_v<U, Order>) return true;
	else if constexpr (StdVectorInfo<U>::is) return ComputeDecodable<typename StdVectorInfo<U>::Value>();
	else if constexpr (StdArrayInfo<U>::is) return ComputeDecodable<typename StdArrayInfo<U>::Value>();
	else if constexpr (StdTupleInfo<U>::is) return TupleDecodable(static_cast<const U *>(nullptr));
	else if constexpr (VariantInfo<U>::is) return VariantDecodable(static_cast<const U *>(nullptr));
	else if constexpr (HasBaseType<U>::value) return ComputeDecodable<typename HasBaseType<U>::Base>();
	else return false;
}

/** Whether the generic decoder can build a value of type T from JSON. */
template <typename T>
inline constexpr bool IsDecodableArg = ComputeDecodable<T>();

/** Whether every argument of a command can be decoded. */
template <typename Ttuple>
struct ArgsDecodable;

/** @copydoc ArgsDecodable */
template <typename... Ts>
struct ArgsDecodable<std::tuple<Ts...>> {
	static constexpr bool value = (IsDecodableArg<Ts> && ...); ///< True when every argument is decodable.
};


/* -- JSON decoding ----------------------------------------------------------------------- */

/**
 * Describe a JSON value for an error message.
 * @param value The value to describe.
 * @return A short description of the JSON type.
 */
std::string DescribeJsonValue(const Json &value)
{
	if (value.is_null()) return "null";
	if (value.is_boolean()) return "a boolean";
	if (value.is_number_unsigned() || value.is_number_integer()) return "an integer";
	if (value.is_number_float()) return "a float";
	if (value.is_string()) return "a string";
	if (value.is_array()) return "an array";
	if (value.is_object()) return "an object";
	return "an unknown value";
}

/**
 * Decode a JSON value into an integral type, rejecting values that do not fit.
 * @param value The JSON value.
 * @param out Receives the decoded value.
 * @param normalized Receives the value as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeIntegral(const Json &value, T &out, Json &normalized, std::string &error)
{
	static_assert(std::is_integral_v<T>);

	if (value.is_number_unsigned()) {
		const uint64_t raw = value.get<uint64_t>();
		if (raw > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
			error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
			return false;
		}
		out = static_cast<T>(raw);
		normalized = out;
		return true;
	}

	if (value.is_number_integer()) {
		const int64_t raw = value.get<int64_t>();
		if constexpr (std::is_signed_v<T>) {
			if (raw < static_cast<int64_t>(std::numeric_limits<T>::min()) || raw > static_cast<int64_t>(std::numeric_limits<T>::max())) {
				error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
				return false;
			}
		} else {
			if (raw < 0 || static_cast<uint64_t>(raw) > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
				error = "value " + fmt::format("{}", raw) + " does not fit in " + TypeName<T>();
				return false;
			}
		}
		out = static_cast<T>(raw);
		normalized = out;
		return true;
	}

	error = "expected an integer, got " + DescribeJsonValue(value);
	return false;
}

/**
 * Decode a JSON value into a bitset of enum values.
 *
 * Accepts either the bits themselves as one integer, or an array of bit indices (which are the
 * enum values). Bits outside the range the enum declares are rejected.
 * @param value The JSON value.
 * @param out Receives the decoded bitset.
 * @param normalized Receives the bits as the catalog reports them back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeBitSet(const Json &value, T &out, Json &normalized, std::string &error)
{
	using Info = EnumBitSetInfo<T>;
	using Storage = typename Info::Storage;
	constexpr uint64_t valid_bits = static_cast<uint64_t>(to_underlying(Info::end));

	uint64_t bits = 0;
	if (value.is_array()) {
		for (const Json &item : value) {
			if (!item.is_number_unsigned() && !item.is_number_integer()) {
				error = "expected integer bit indices, got " + DescribeJsonValue(item);
				return false;
			}
			if (item.is_number_integer() && item.get<int64_t>() < 0) {
				error = "bit index " + fmt::format("{}", item.get<int64_t>()) + " is negative";
				return false;
			}
			const uint64_t bit = item.get<uint64_t>();
			if (bit >= valid_bits) {
				error = "bit index " + fmt::format("{}", bit) + " is outside the enum (0.." + fmt::format("{}", valid_bits - 1) + ")";
				return false;
			}
			bits |= (uint64_t{1} << bit);
		}
	} else if (value.is_number_unsigned() || value.is_number_integer()) {
		if (value.is_number_integer() && value.get<int64_t>() < 0) {
			error = "bitset value " + fmt::format("{}", value.get<int64_t>()) + " is negative";
			return false;
		}
		bits = value.get<uint64_t>();
		if (bits > static_cast<uint64_t>(std::numeric_limits<Storage>::max())) {
			error = "bitset value " + fmt::format("{}", bits) + " does not fit in " + TypeName<T>();
			return false;
		}
		if (valid_bits < static_cast<uint64_t>(std::numeric_limits<Storage>::digits) && (bits >> valid_bits) != 0) {
			error = "bitset value " + fmt::format("{}", bits) + " sets bits outside the enum (0.." + fmt::format("{}", valid_bits - 1) + ")";
			return false;
		}
	} else {
		error = "expected a bitset integer or an array of bit indices, got " + DescribeJsonValue(value);
		return false;
	}

	out = T{static_cast<Storage>(bits)};
	normalized = static_cast<Storage>(bits);
	return true;
}

/**
 * Decode the elements of a tuple argument.
 * @param value The JSON array to decode.
 * @param out Receives the decoded tuple.
 * @param elements Receives the normalized elements, already sized to the tuple.
 * @param error Receives the reason when a value is rejected.
 * @param ok Set to false when a value is rejected.
 */
template <typename Ttuple, size_t... I>
void DecodeTupleElements(const Json &value, Ttuple &out, Json &elements, std::string &error, bool &ok, std::index_sequence<I...>);

/**
 * Decode a JSON value into a command argument.
 * @param value The JSON value.
 * @param out Receives the decoded argument.
 * @param normalized Receives the argument as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded; false when the value or the type is not supported.
 */
template <typename T>
bool DecodeArg(const Json &value, T &out, Json &normalized, std::string &error);

/* -- decoders of the engine's structured argument types ---------------------------------- */


/**
 * Decode one parameter of an encoded string.
 * @param value A string, an integer, {"text": string} or {"id": integer}.
 * @param out Receives the parameter.
 * @param normalized Receives the parameter as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the parameter was decoded.
 */
bool DecodeStringParameter(const Json &value, StringParameter &out, Json &normalized, std::string &error)
{
	if (value.is_string()) {
		out = StringParameter(value.get<std::string>());
		normalized = value;
		return true;
	}
	if (value.is_number_unsigned() || value.is_number_integer()) {
		uint64_t raw = 0;
		if (!DecodeIntegral<uint64_t>(value, raw, normalized, error)) return false;
		out = StringParameter(raw);
		return true;
	}
	if (value.is_object()) {
		if (value.contains("text")) {
			if (!value["text"].is_string()) {
				error = "'text' of a string parameter must be a string";
				return false;
			}
			const std::string text = value["text"].get<std::string>();
			out = StringParameter(text);
			normalized = {{"text", text}};
			return true;
		}
		if (value.contains("id")) {
			uint64_t raw = 0;
			Json ignored;
			if (!DecodeIntegral<uint64_t>(value["id"], raw, ignored, error)) return false;
			out = StringParameter(raw);
			normalized = {{"id", raw}};
			return true;
		}
	}
	error = "a string parameter is a string, an integer, {\"text\": string} or {\"id\": integer}, got " + DescribeJsonValue(value);
	return false;
}

/**
 * Decode an EncodedString.
 *
 * A literal text is encoded through the engine's own raw-text string (STR_JUST_STRING) with the text
 * as its parameter, so it can never be read as a control code. A structured string is a StringID
 * plus its parameters.
 * @param value A literal text, or {"text": string} or {"string_id": N, "params": [...]}.
 * @param out Receives the encoded string.
 * @param normalized Receives the string as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the string was decoded.
 */
bool DecodeEncodedString(const Json &value, EncodedString &out, Json &normalized, std::string &error)
{
	if (value.is_string()) {
		const std::string text = value.get<std::string>();
		const std::array<StringParameter, 1> params{StringParameter(text)};
		out = GetEncodedStringWithArgs(STR_JUST_STRING, params);
		normalized = {{"text", text}};
		return true;
	}
	if (!value.is_object()) {
		error = "an EncodedString is a literal text or {\"string_id\": N, \"params\": [...]}, got " + DescribeJsonValue(value);
		return false;
	}

	const bool has_text = value.contains("text");
	const bool has_id = value.contains("string_id");
	if (has_text == has_id) {
		error = "an EncodedString needs exactly one of 'text' and 'string_id'";
		return false;
	}

	std::vector<StringParameter> params;
	Json normalized_params = Json::array();
	if (value.contains("params")) {
		if (!value["params"].is_array()) {
			error = "'params' must be an array";
			return false;
		}
		for (const Json &item : value["params"]) {
			StringParameter param;
			Json normalized_param;
			if (!DecodeStringParameter(item, param, normalized_param, error)) return false;
			params.push_back(std::move(param));
			normalized_params.push_back(std::move(normalized_param));
		}
	}

	if (has_text) {
		if (!value["text"].is_string()) {
			error = "'text' must be a string";
			return false;
		}
		if (!params.empty()) {
			error = "a literal 'text' takes no 'params'; use 'string_id' for a parameterised string";
			return false;
		}
		const std::string text = value["text"].get<std::string>();
		const std::array<StringParameter, 1> params{StringParameter(text)};
		out = GetEncodedStringWithArgs(STR_JUST_STRING, params);
		normalized = {{"text", text}};
		return true;
	}

	uint32_t string_id = 0;
	Json ignored;
	if (!DecodeIntegral<uint32_t>(value["string_id"], string_id, ignored, error)) return false;
	out = params.empty() ? GetEncodedString(StringID{string_id}) : GetEncodedStringWithArgs(StringID{string_id}, params);
	normalized = {{"string_id", string_id}, {"params", std::move(normalized_params)}};
	return true;
}

/**
 * Decode a subsidy Source: a town, an industry or a company headquarters.
 * @param value {"type": "town"|"industry"|"headquarters", "id": N}.
 * @param out Receives the source.
 * @param normalized Receives the source as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the source was decoded.
 */
bool DecodeSource(const Json &value, Source &out, Json &normalized, std::string &error)
{
	if (!value.is_object()) {
		error = "a Source is {\"type\": \"town\"|\"industry\"|\"headquarters\", \"id\": N}, got " + DescribeJsonValue(value);
		return false;
	}
	if (!value.contains("type") || !value["type"].is_string()) {
		error = "a Source needs 'type': town, industry or headquarters";
		return false;
	}
	const std::string type = value["type"].get<std::string>();
	SourceType source_type;
	if (type == "town") {
		source_type = SourceType::Town;
	} else if (type == "industry") {
		source_type = SourceType::Industry;
	} else if (type == "headquarters") {
		source_type = SourceType::Headquarters;
	} else {
		error = "Source 'type' must be town, industry or headquarters, got " + type;
		return false;
	}
	if (!value.contains("id")) {
		error = "a Source needs 'id'";
		return false;
	}
	uint16_t id = 0;
	Json ignored;
	if (!DecodeIntegral<uint16_t>(value["id"], id, ignored, error)) return false;
	if (id == Source::Invalid) {
		error = "Source id 65535 is the invalid marker";
		return false;
	}
	out = Source(id, source_type);
	normalized = {{"type", type}, {"id", id}};
	return true;
}

/** The vehicle list types the catalog accepts, with the JSON name of each. */
struct VehicleListTypeName {
	const char *name; ///< The catalog name.
	VehicleListType type; ///< The engine value.
};

/** All vehicle list types. */
constexpr VehicleListTypeName _vehicle_list_type_names[] = {
	{"company", VehicleListType::Company},
	{"vehicle_shared_orders", VehicleListType::VehicleSharedOrders},
	{"station", VehicleListType::Station},
	{"depot", VehicleListType::Depot},
	{"group", VehicleListType::Group},
};

/**
 * Decode a vehicle list identifier.
 * @param value {"list": name, "vtype": VehicleType, "company": CompanyID, "index": N}.
 * @param out Receives the identifier.
 * @param normalized Receives the identifier as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the identifier was decoded.
 */
bool DecodeVehicleListIdentifier(const Json &value, VehicleListIdentifier &out, Json &normalized, std::string &error)
{
	if (!value.is_object()) {
		error = "a VehicleListIdentifier is {\"list\": name, \"vtype\": N, \"company\": N, \"index\": N}, got " + DescribeJsonValue(value);
		return false;
	}
	if (!value.contains("list") || !value["list"].is_string()) {
		error = "a vehicle list needs 'list': company, vehicle_shared_orders, station, depot or group";
		return false;
	}
	const std::string list = value["list"].get<std::string>();
	VehicleListType list_type = VehicleListType::End;
	for (const VehicleListTypeName &entry : _vehicle_list_type_names) {
		if (list == entry.name) list_type = entry.type;
	}
	if (list_type == VehicleListType::End) {
		error = "unknown vehicle list '" + list + "'";
		return false;
	}

	Json ignored;
	if (!value.contains("vtype")) {
		error = "a vehicle list needs 'vtype'";
		return false;
	}
	uint8_t vtype = 0;
	if (!DecodeIntegral<uint8_t>(value["vtype"], vtype, ignored, error)) return false;
	if (vtype >= to_underlying(VehicleType::CompanyEnd)) {
		error = "vehicle list 'vtype' must be a company owned vehicle type";
		return false;
	}

	if (!value.contains("company")) {
		error = "a vehicle list needs 'company'";
		return false;
	}
	CompanyID company{};
	if (!DecodeArg(value["company"], company, ignored, error)) return false;

	if (!value.contains("index")) {
		error = "a vehicle list needs 'index'";
		return false;
	}
	uint32_t index = 0;
	if (!DecodeIntegral<uint32_t>(value["index"], index, ignored, error)) return false;

	out = VehicleListIdentifier{list_type, static_cast<VehicleType>(vtype), company, index};
	normalized = {
		{"list", list},
		{"vtype", vtype},
		{"company", static_cast<uint64_t>(company.base())},
		{"index", index},
	};
	return true;
}

/** The order types the catalog accepts, with the JSON name of each. */
struct OrderTypeName {
	const char *name; ///< The catalog name.
	OrderType type; ///< The engine value.
};

/** All order types the catalog builds. */
constexpr OrderTypeName _order_type_names[] = {
	{"goto_station", OT_GOTO_STATION},
	{"goto_waypoint", OT_GOTO_WAYPOINT},
	{"goto_depot", OT_GOTO_DEPOT},
	{"loading", OT_LOADING},
	{"leave_station", OT_LEAVESTATION},
	{"implicit", OT_IMPLICIT},
	{"dummy", OT_DUMMY},
};

/** The catalog name of one alternative of a variant argument. */
template <typename T>
std::string VariantAlternativeName()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, std::monostate>) {
		return "none";
	} else if constexpr (std::is_same_v<U, TileIndex>) {
		return "tile";
	} else {
		const Json target = TargetSchema<U>();
		if (target.is_object() && target.contains("entity")) return target["entity"].get<std::string>();
		return TypeName<U>();
	}
}

/** The names of all alternatives of a variant. */
template <typename... Ts>
Json VariantNames(const std::variant<Ts...> *)
{
	Json names = Json::array();
	(names.push_back(VariantAlternativeName<Ts>()), ...);
	return names;
}

/** Decode the value of the variant alternative that was named. */
template <typename T, typename... Ts>
void DecodeVariantAlternative(const Json &payload, std::variant<Ts...> &out, Json &normalized, std::string &error, bool &matched)
{
	using U = std::remove_cvref_t<T>;
	matched = true;
	if constexpr (std::is_same_v<U, std::monostate>) {
		if (!payload.is_null()) {
			error = "variant alternative 'none' takes no value";
			return;
		}
		out = std::monostate{};
		normalized = {{"type", "none"}};
	} else {
		U decoded{};
		Json normalized_value;
		if (!DecodeArg(payload, decoded, normalized_value, error)) return;
		out = std::move(decoded);
		normalized = {{"type", VariantAlternativeName<U>()}, {"value", std::move(normalized_value)}};
	}
}

/**
 * Decode a variant argument.
 *
 * The value names the alternative and carries its value: `{"type": name, "value": value}`, or
 * `{"type": "none"}` for the empty alternative.
 * @param value The JSON value.
 * @param out Receives the variant.
 * @param normalized Receives the variant as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the variant was decoded.
 */
template <typename... Ts>
bool DecodeVariant(const Json &value, std::variant<Ts...> &out, Json &normalized, std::string &error)
{
	if (!value.is_object() || !value.contains("type") || !value["type"].is_string()) {
		error = "a variant argument is {\"type\": name, \"value\": value}, got " + DescribeJsonValue(value);
		return false;
	}
	const std::string type = value["type"].get<std::string>();
	const Json payload = value.contains("value") ? value["value"] : Json(nullptr);

	bool matched = false;
	((!matched && type == VariantAlternativeName<Ts>() ? (DecodeVariantAlternative<Ts>(payload, out, normalized, error, matched), void()) : void()), ...);
	if (!matched) {
		error = "unknown variant type '" + type + "', expected one of " + VariantNames(static_cast<const std::variant<Ts...> *>(nullptr)).dump();
		return false;
	}
	return error.empty();
}

/**
 * Decode an Order.
 *
 * The order is built with the engine's own Make* helpers, so its packed type/flags bytes are always
 * the ones the engine writes. Unknown fields are rejected instead of ignored, so a typo cannot
 * silently build a different order than the caller meant.
 * @param value {"type": name, "destination": N, plus the optional fields of that order type}.
 * @param out Receives the order.
 * @param normalized Receives the order as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the order was decoded.
 */
bool DecodeOrder(const Json &value, Order &out, Json &normalized, std::string &error)
{
	if (!value.is_object()) {
		error = "an Order is an object with 'type' and its destination, got " + DescribeJsonValue(value);
		return false;
	}
	if (!value.contains("type") || !value["type"].is_string()) {
		error = "an Order needs 'type'";
		return false;
	}
	const std::string type_name = value["type"].get<std::string>();
	OrderType type = OT_END;
	for (const OrderTypeName &entry : _order_type_names) {
		if (type_name == entry.name) type = entry.type;
	}
	if (type == OT_END) {
		error = "unknown order type '" + type_name + "'";
		return false;
	}

	const bool needs_destination = type == OT_GOTO_STATION || type == OT_GOTO_WAYPOINT || type == OT_GOTO_DEPOT || type == OT_IMPLICIT;
	const bool takes_depot_flags = type == OT_GOTO_DEPOT;
	const bool takes_ordered = type == OT_LOADING;

	static constexpr std::string_view common_fields[] = {
		"non_stop", "stop_location", "load", "unload", "refit_cargo", "wait_time", "travel_time", "max_speed",
	};
	for (const auto &member : value.items()) {
		const std::string &key = member.key();
		if (key == "type") continue;
		if (key == "destination" || key == "destination_entity") {
			if (!needs_destination) {
				error = "order type '" + type_name + "' takes no 'destination'";
				return false;
			}
			continue;
		}
		if (key == "depot_order" || key == "action") {
			if (!takes_depot_flags) {
				error = "order type '" + type_name + "' takes no '" + key + "'";
				return false;
			}
			continue;
		}
		if (key == "ordered") {
			if (!takes_ordered) {
				error = "order type '" + type_name + "' takes no 'ordered'";
				return false;
			}
			continue;
		}
		bool known = false;
		for (const std::string_view &field : common_fields) {
			if (key == field) known = true;
		}
		if (!known) {
			error = "unknown order field '" + key + "'";
			return false;
		}
	}

	Json ignored;
	std::string destination_entity;
	uint16_t destination = 0;
	if (needs_destination) {
		if (!value.contains("destination")) {
			error = "order type '" + type_name + "' needs 'destination'";
			return false;
		}
		if (!DecodeIntegral<uint16_t>(value["destination"], destination, ignored, error)) return false;
		if (type == OT_GOTO_DEPOT) {
			destination_entity = "depot";
		} else if (type == OT_GOTO_WAYPOINT) {
			destination_entity = "waypoint";
		} else {
			destination_entity = "station";
		}

		/* A hangar is a station, not a depot, so a depot order may say what its destination is. */
		if (value.contains("destination_entity")) {
			if (!value["destination_entity"].is_string()) {
				error = "'destination_entity' must be a string";
				return false;
			}
			const std::string requested = value["destination_entity"].get<std::string>();
			const bool allowed = type == OT_GOTO_DEPOT
				? (requested == "depot" || requested == "station")
				: (requested == "station" || (type == OT_GOTO_WAYPOINT && requested == "waypoint"));
			if (!allowed) {
				error = "order type '" + type_name + "' cannot have destination_entity '" + requested + "'";
				return false;
			}
			destination_entity = requested;
		}
	}

	Order order;
	switch (type) {
		case OT_GOTO_STATION:
			order.MakeGoToStation(StationID{destination});
			break;
		case OT_GOTO_WAYPOINT:
			order.MakeGoToWaypoint(StationID{destination});
			break;
		case OT_GOTO_DEPOT: {
			OrderDepotTypeFlags depot_order = OrderDepotTypeFlag::PartOfOrders;
			OrderDepotActionFlags action{};
			if (value.contains("depot_order") && !DecodeArg(value["depot_order"], depot_order, ignored, error)) return false;
			if (value.contains("action") && !DecodeArg(value["action"], action, ignored, error)) return false;
			order.MakeGoToDepot(DestinationID(destination), depot_order, OrderNonStopFlag::NonStop, action);
			break;
		}
		case OT_LOADING: {
			bool ordered = false;
			if (value.contains("ordered") && !DecodeArg(value["ordered"], ordered, ignored, error)) return false;
			order.MakeLoading(ordered);
			break;
		}
		case OT_LEAVESTATION:
			order.MakeLeaveStation();
			break;
		case OT_IMPLICIT:
			order.MakeImplicit(StationID{destination});
			break;
		case OT_DUMMY:
			order.MakeDummy();
			break;
		default:
			break;
	}

	if (value.contains("non_stop")) {
		OrderNonStopFlags non_stop{};
		if (!DecodeArg(value["non_stop"], non_stop, ignored, error)) return false;
		order.SetNonStopType(non_stop);
	}
	if (value.contains("stop_location")) {
		OrderStopLocation stop_location = OrderStopLocation::NearEnd;
		if (!DecodeArg(value["stop_location"], stop_location, ignored, error)) return false;
		order.SetStopLocation(stop_location);
	}
	if (value.contains("load")) {
		OrderLoadType load = OrderLoadType::LoadIfPossible;
		if (!DecodeArg(value["load"], load, ignored, error)) return false;
		order.SetLoadType(load);
	}
	if (value.contains("unload")) {
		OrderUnloadType unload = OrderUnloadType::UnloadIfPossible;
		if (!DecodeArg(value["unload"], unload, ignored, error)) return false;
		order.SetUnloadType(unload);
	}
	if (value.contains("refit_cargo")) {
		CargoType cargo = CARGO_NO_REFIT;
		if (!DecodeArg(value["refit_cargo"], cargo, ignored, error)) return false;
		order.SetRefit(cargo);
	}
	if (value.contains("wait_time")) {
		uint16_t wait = 0;
		if (!DecodeIntegral<uint16_t>(value["wait_time"], wait, ignored, error)) return false;
		order.SetWaitTime(wait);
	}
	if (value.contains("travel_time")) {
		uint16_t travel = 0;
		if (!DecodeIntegral<uint16_t>(value["travel_time"], travel, ignored, error)) return false;
		order.SetTravelTime(travel);
	}
	if (value.contains("max_speed")) {
		uint16_t speed = 0;
		if (!DecodeIntegral<uint16_t>(value["max_speed"], speed, ignored, error)) return false;
		order.SetMaxSpeed(speed);
	}

	out = order;
	normalized = value;
	if (needs_destination) normalized["destination_entity"] = destination_entity;
	return true;
}

/**
 * Decode a JSON value into a command argument.
 * @param value The JSON value.
 * @param out Receives the decoded argument.
 * @param normalized Receives the argument as the catalog reports it back.
 * @param error Receives the reason when the value is rejected.
 * @return True when the value was decoded; false when the value or the type is not supported.
 */
template <typename T>
bool DecodeArg(const Json &value, T &out, Json &normalized, std::string &error)
{
	using U = std::remove_cvref_t<T>;

	if constexpr (std::is_same_v<U, bool>) {
		if (!value.is_boolean()) {
			error = "expected a boolean, got " + DescribeJsonValue(value);
			return false;
		}
		out = value.get<bool>();
		normalized = out;
		return true;
	} else if constexpr (std::is_integral_v<U>) {
		return DecodeIntegral<U>(value, out, normalized, error);
	} else if constexpr (std::is_enum_v<U>) {
		using Base = std::underlying_type_t<U>;
		Base decoded{};
		if (!DecodeArg<Base>(value, decoded, normalized, error)) return false;
		/* Where the engine declares an end marker, only the enum's own values are accepted. */
		if constexpr (EnumEndInfo<U>::is) {
			if (static_cast<int64_t>(decoded) < 0 || static_cast<uint64_t>(decoded) >= EnumEndInfo<U>::end) {
				error = "value " + fmt::format("{}", static_cast<int64_t>(decoded)) + " is not a valid " + TypeName<U>() +
					" (0.." + fmt::format("{}", EnumEndInfo<U>::end - 1) + ")";
				return false;
			}
		}
		out = static_cast<U>(decoded);
		return true;
	} else if constexpr (EnumBitSetInfo<U>::is) {
		return DecodeBitSet<U>(value, out, normalized, error);
	} else if constexpr (std::is_same_v<U, std::string>) {
		if (!value.is_string()) {
			error = "expected a string, got " + DescribeJsonValue(value);
			return false;
		}
		out = value.get<std::string>();
		normalized = out;
		return true;
	} else if constexpr (StdVectorInfo<U>::is) {
		if (!value.is_array()) {
			error = "expected an array, got " + DescribeJsonValue(value);
			return false;
		}
		using Value = typename StdVectorInfo<U>::Value;
		out.clear();
		Json elements = Json::array();
		for (const Json &item : value) {
			Value element{};
			Json element_normalized;
			if (!DecodeArg(item, element, element_normalized, error)) return false;
			out.push_back(std::move(element));
			elements.push_back(std::move(element_normalized));
		}
		normalized = std::move(elements);
		return true;
	} else if constexpr (StdArrayInfo<U>::is) {
		if (!value.is_array() || value.size() != StdArrayInfo<U>::size) {
			error = "expected an array of " + fmt::format("{}", StdArrayInfo<U>::size) + " elements, got " + DescribeJsonValue(value);
			return false;
		}
		using Value = typename StdArrayInfo<U>::Value;
		Json elements = Json::array();
		bool ok = true;
		size_t index = 0;
		for (const Json &item : value) {
			Value element{};
			Json element_normalized;
			ok = ok && DecodeArg(item, element, element_normalized, error);
			if (!ok) return false;
			out[index++] = std::move(element);
			elements.push_back(std::move(element_normalized));
		}
		normalized = std::move(elements);
		return true;
	} else if constexpr (StdTupleInfo<U>::is) {
		constexpr size_t count = std::tuple_size_v<U>;
		if (!value.is_array() || value.size() != count) {
			error = "expected an array of " + fmt::format("{}", count) + " elements, got " + DescribeJsonValue(value);
			return false;
		}
		Json elements = Json::array();
		bool ok = true;
		DecodeTupleElements(value, out, elements, error, ok, std::make_index_sequence<count>{});
		if (!ok) return false;
		normalized = std::move(elements);
		return true;
	} else if constexpr (std::is_same_v<U, EncodedString>) {
		return DecodeEncodedString(value, out, normalized, error);
	} else if constexpr (std::is_same_v<U, Source>) {
		return DecodeSource(value, out, normalized, error);
	} else if constexpr (std::is_same_v<U, VehicleListIdentifier>) {
		return DecodeVehicleListIdentifier(value, out, normalized, error);
	} else if constexpr (std::is_same_v<U, Order>) {
		return DecodeOrder(value, out, normalized, error);
	} else if constexpr (VariantInfo<U>::is) {
		return DecodeVariant(value, out, normalized, error);
	} else if constexpr (HasBaseType<U>::value) {
		using Base = typename HasBaseType<U>::Base;
		Base decoded{};
		if (!DecodeArg<Base>(value, decoded, normalized, error)) return false;
		out = U{decoded};
		return true;
	} else {
		error = "argument type " + TypeName<U>() + " has no JSON decoding";
		return false;
	}
}

/**
 * Decode the elements of a tuple argument.
 * @param value The JSON array to decode.
 * @param out Receives the decoded tuple.
 * @param elements Receives the normalized elements, already sized to the tuple.
 * @param error Receives the reason when a value is rejected.
 * @param ok Set to false when a value is rejected.
 */
template <typename Ttuple, size_t... I>
void DecodeTupleElements(const Json &value, Ttuple &out, Json &elements, std::string &error, bool &ok, std::index_sequence<I...>)
{
	Json element;
	(((ok = ok && DecodeArgAt(value, I, std::get<I>(out), element, error)), elements.push_back(std::move(element))), ...);
}

/**
 * Decode one command argument at its position in the argument array.
 * @param args The JSON array of arguments.
 * @param index The position of the argument.
 * @param out Receives the decoded argument.
 * @param normalized Receives the argument as the catalog reports it back.
 * @param error Receives the reason when the value is rejected, prefixed with the position.
 * @return True when the value was decoded.
 */
template <typename T>
bool DecodeArgAt(const Json &args, size_t index, T &out, Json &normalized, std::string &error)
{
	std::string detail;
	if (!DecodeArg(args[index], out, normalized, detail)) {
		error = "argument " + fmt::format("{}", index) + ": " + detail;
		return false;
	}
	return true;
}

/**
 * Decode the ordered arguments of a command.
 * @param args The JSON array of arguments.
 * @param out Receives the decoded arguments.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected.
 * @return True when the arguments were decoded.
 */
template <typename Ttuple, size_t... I>
bool DecodeArgsAt(const Json &args, Ttuple &out, Json &normalized, std::string &error, std::index_sequence<I...>)
{
	normalized = Json::array();
	bool ok = true;
	std::string detail;
	Json element;
	(((ok = ok && DecodeArgAt(args, I, std::get<I>(out), element, detail)), normalized.push_back(std::move(element))), ...);
	if (!ok) error = detail;
	return ok;
}

/**
 * Check the shape of the argument array and decode it.
 * @param args The JSON array of arguments.
 * @param out Receives the decoded arguments.
 * @param normalized Receives the arguments as the catalog reports them back.
 * @param error Receives the reason when the arguments are rejected.
 * @return True when the arguments were decoded.
 */
template <typename Ttuple>
bool DecodeArgs(const Json &args, Ttuple &out, Json &normalized, std::string &error)
{
	constexpr size_t count = std::tuple_size_v<Ttuple>;
	if (!args.is_array()) {
		error = "command arguments must be a JSON array, got " + DescribeJsonValue(args);
		return false;
	}
	if (args.size() != count) {
		error = "command takes " + fmt::format("{}", count) + " argument(s), got " + fmt::format("{}", args.size());
		return false;
	}
	return DecodeArgsAt(args, out, normalized, error, std::make_index_sequence<count>{});
}

/* -- catalog metadata -------------------------------------------------------------------- */

/** The catalog name of a command type. */
std::string_view CommandTypeName(CommandType type)
{
	static constexpr std::string_view names[] = {
		"LandscapeConstruction", "VehicleConstruction", "MoneyManagement", "VehicleManagement",
		"RouteManagement", "OtherManagement", "CompanySetting", "ServerSetting", "Cheat",
	};
	static_assert(std::size(names) == to_underlying(CommandType::End));
	const size_t index = to_underlying(type);
	return index < std::size(names) ? names[index] : "Unknown";
}

/** One command flag and the name the catalog uses for it. */
struct CommandFlagName {
	CommandFlag flag; ///< The flag.
	std::string_view name; ///< The catalog name of the flag.
};

/** All command flags, in declaration order. */
constexpr CommandFlagName _command_flag_names[] = {
	{CommandFlag::Server, "Server"},
	{CommandFlag::Spectator, "Spectator"},
	{CommandFlag::Offline, "Offline"},
	{CommandFlag::Auto, "Auto"},
	{CommandFlag::AllTiles, "AllTiles"},
	{CommandFlag::NoTest, "NoTest"},
	{CommandFlag::NoWater, "NoWater"},
	{CommandFlag::ClientID, "ClientID"},
	{CommandFlag::Deity, "Deity"},
	{CommandFlag::StrCtrl, "StrCtrl"},
	{CommandFlag::NoEst, "NoEst"},
	{CommandFlag::Location, "Location"},
};

/**
 * List the flags of a command.
 * @param flags The flags of the command.
 * @return The catalog names of the flags that are set.
 */
Json FlagsJson(CommandFlags flags)
{
	Json names = Json::array();
	for (const CommandFlagName &entry : _command_flag_names) {
		if (flags.Test(entry.flag)) names.push_back(std::string(entry.name));
	}
	return names;
}

/**
 * Describe which companies may issue a command, as far as the flags say.
 * @param flags The flags of the command.
 * @return The company constraints of the command.
 */
Json CompanyConstraints(CommandFlags flags)
{
	return {
		{"server_only", flags.Test(CommandFlag::Server)},
		{"spectator_allowed", flags.Test(CommandFlag::Spectator)},
		{"deity_allowed", flags.Test(CommandFlag::Deity)},
		{"offline_only", flags.Test(CommandFlag::Offline)},
		{"needs_client_id", flags.Test(CommandFlag::ClientID)},
	};
}

/**
 * The command name without the `Cmd` prefix.
 * @param name The proc name of the command.
 * @return The alias the catalog also accepts.
 */
std::string CommandAlias(std::string_view name)
{
	if (name.starts_with("Cmd")) name.remove_prefix(3);
	return std::string(name);
}

/**
 * Why a command's arguments cannot be decoded yet.
 * @return A description of the arguments that have no JSON decoding, empty when all are supported.
 */
template <typename... Ts>
std::string UnsupportedArgsReason(const std::tuple<Ts...> *)
{
	std::string reason;
	size_t index = 0;
	const auto add = [&reason](size_t position, const std::string &type) {
		if (!reason.empty()) reason += "; ";
		reason += "argument " + fmt::format("{}", position) + " is " + type + " which has no JSON decoding yet";
	};
	(((IsDecodableArg<Ts> ? void() : add(index, TypeName<Ts>())), ++index), ...);
	return reason;
}

/** Where the entity id of one argument lives; defined with the command targets below. */
template <typename T>
Json TargetSchema();

/**
 * The exact JSON schema of one argument, for the structured types the decoder understands.
 * @return The schema of the argument, or null when the argument is a plain value.
 */
template <typename T>
Json ArgumentSchema()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, EncodedString>) {
		return {
			{"one_of", Json::array({
				Json{{"literal_text", "a plain JSON string; encoded as the engine's raw-text string with the text as its parameter"}},
				Json{{"string_id", "StringID"}, {"params", "array of string | integer | {\"text\": string} | {\"id\": integer}"}},
			})},
		};
	} else if constexpr (std::is_same_v<U, Order>) {
		return {
			{"type", Json::array({"goto_station", "goto_waypoint", "goto_depot", "loading", "leave_station", "implicit", "dummy"})},
			{"destination", "StationID for goto_station, goto_waypoint and implicit; DepotID for goto_depot"},
			{"destination_entity", "optional \"station\"|\"depot\"|\"waypoint\": what the destination id is; an aircraft hangar is a station, so a goto_depot order to a hangar says \"station\""},
			{"optional", Json::array({"non_stop", "stop_location", "load", "unload", "refit_cargo", "wait_time", "travel_time", "max_speed", "depot_order", "action", "ordered"})},
			{"rejects_unknown_fields", true},
			{"built_with", "the engine's own Order::Make* helpers"},
		};
	} else if constexpr (std::is_same_v<U, Source>) {
		return {
			{"type", Json::array({"town", "industry", "headquarters"})},
			{"id", "uint16; 65535 is the invalid marker and is rejected"},
		};
	} else if constexpr (std::is_same_v<U, VehicleListIdentifier>) {
		return {
			{"list", Json::array({"company", "vehicle_shared_orders", "station", "depot", "group"})},
			{"vtype", "VehicleType below CompanyEnd"},
			{"company", "CompanyID"},
			{"index", "the id the list is about: company, vehicle, station, depot or group"},
		};
	} else if constexpr (VariantInfo<U>::is) {
		return {
			{"type", VariantNames(static_cast<const U *>(nullptr))},
			{"value", "the value of the chosen alternative; 'none' takes no value"},
		};
	} else if constexpr (std::is_enum_v<U>) {
		if constexpr (EnumEndInfo<U>::is) {
			return {{"values", "0.." + fmt::format("{}", EnumEndInfo<U>::end - 1) + ", checked against the engine's own end marker"}};
		} else {
			return {{"values", "the underlying integer range; this enum declares no end marker to check against"}};
		}
	} else if constexpr (EnumBitSetInfo<U>::is) {
		return {{"bits", "0.." + fmt::format("{}", to_underlying(EnumBitSetInfo<U>::end) - 1) + ", checked against the engine's own bit set"}};
	} else {
		return nullptr;
	}
}

/**
 * Describe one argument of a command.
 * @param out Receives the argument entry.
 * @param targets Receives the target descriptor, when the argument names an entity.
 * @param index The position of the argument.
 */
template <typename T>
void DescribeArg(Json &out, Json &targets, size_t index)
{
	const Json target = TargetSchema<T>();
	out.push_back(Json{
		{"index", index},
		{"name", nullptr},
		{"type", TypeName<T>()},
		{"kind", std::string(ArgKind<T>())},
		{"supported", IsDecodableArg<T>},
		{"target", target},
		{"schema", ArgumentSchema<T>()},
	});
	if (!target.is_null()) targets.push_back({{"index", index}, {"target", target}});
}

/**
 * Describe the arguments of a command.
 * @param out Receives one entry per argument, in call order.
 * @param targets Receives one entry per argument that names an entity.
 */
template <typename... Ts, size_t... I>
void DescribeArgs(const std::tuple<Ts...> *, Json &out, Json &targets, std::index_sequence<I...>)
{
	(DescribeArg<Ts>(out, targets, I), ...);
}

/**
 * List the extra values a command proc returns.
 * @return One entry per extra return value, empty when the proc only returns a CommandCost.
 */
template <typename... Ts>
Json ReturnNames(const std::tuple<Ts...> *)
{
	Json names = Json::array();
	(names.push_back(TypeName<Ts>()), ...);
	return names;
}

/**
 * The contract the backend derives its expected refs from.
 *
 * Kept out of the per-command template: the text and the rule are the same for every command, and
 * every instantiation would otherwise carry its own copy.
 * @param targets The entities the arguments of the command name.
 * @return The machine readable expected refs contract of the command.
 */
Json ExpectedRefsMetadata(const Json &targets)
{
	return {
		{"required_for_write", true},
		{"set_equality", true},
		{"includes_actor_company", true},
		{"rule", "expected_targets must name exactly these {entity, id} pairs; a missing pair, an extra pair or a different id is rejected before Post"},
		{"sources", targets},
	};
}

/**
 * Build the catalog entry of a command.
 * @param full Whether to include the argument schema and the return values.
 * @return The catalog entry.
 */
template <Commands Tcmd>
Json DescribeCommand(bool full);

/* -- command targets ---------------------------------------------------------------------- */

/** Add the target of one variant alternative to the entity map of the variant. */
template <typename T>
void AddVariantTarget(Json &map)
{
	const Json target = TargetSchema<T>();
	if (target.is_object() && target.contains("entity")) map[VariantAlternativeName<T>()] = target["entity"];
}

/** The target schema of a variant argument: the id sits in 'value', its kind in 'type'. */
template <typename... Ts>
Json VariantTargetSchema(const std::variant<Ts...> *)
{
	Json map = Json::object();
	(AddVariantTarget<Ts>(map), ...);
	if (map.empty()) return nullptr;
	return {{"entity_by", {{"field", "type"}, {"map", map}}}, {"id_path", "value"}};
}

/**
 * Where the entity id of one argument lives, so the backend can derive its expected refs.
 *
 * `{"entity": kind, "id_path": null}` means the argument value itself is the id of that kind.
 * `{"entity_by": {"field": field, "map": {value: kind}}, "id_path": field}` means the id sits in
 * `id_path` of the argument object and the entity kind is decided by `entity_by.field` through the
 * map. The rule is the same for every argument, so a consumer needs no per-type code.
 * @return The target schema of the argument type, or null when the argument names no entity.
 */
template <typename T>
Json TargetSchema()
{
	using U = std::remove_cvref_t<T>;
	if constexpr (std::is_same_v<U, TownID>) return {{"entity", "town"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, StationID>) return {{"entity", "station"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, VehicleID>) return {{"entity", "vehicle"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, IndustryID>) return {{"entity", "industry"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, GroupID>) return {{"entity", "group"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, SignID>) return {{"entity", "sign"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, DepotID>) return {{"entity", "depot"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, CompanyID>) return {{"entity", "company"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, EngineID>) return {{"entity", "engine"}, {"id_path", nullptr}};
	else if constexpr (std::is_same_v<U, Order>) {
		/* Waypoints live in the station pool, so they guard as a station. */
		return {
			{"entity_by", {
				{"field", "destination_entity"},
				{"map", {{"station", "station"}, {"depot", "depot"}, {"waypoint", "station"}}},
			}},
			{"id_path", "destination"},
		};
	} else if constexpr (std::is_same_v<U, Source>) {
		return {
			{"entity_by", {
				{"field", "type"},
				{"map", {{"town", "town"}, {"industry", "industry"}, {"headquarters", "company"}}},
			}},
			{"id_path", "id"},
		};
	} else if constexpr (std::is_same_v<U, VehicleListIdentifier>) {
		return {
			{"entity_by", {
				{"field", "list"},
				{"map", {
					{"company", "company"},
					{"depot", "depot"},
					{"group", "group"},
					{"station", "station"},
					{"vehicle_shared_orders", "vehicle"},
				}},
			}},
			{"id_path", "index"},
		};
	} else if constexpr (VariantInfo<U>::is) {
		return VariantTargetSchema(static_cast<const U *>(nullptr));
	} else {
		return nullptr;
	}
}

/**
 * Resolve the entity an argument names.
 * @param schema The target schema of the argument type.
 * @param value The normalized value of the argument.
 * @param entity Receives the entity kind.
 * @param id Receives the id of the entity.
 * @return True when the argument names an entity.
 */
bool ResolveTarget(const Json &schema, const Json &value, std::string &entity, uint64_t &id)
{
	if (!schema.is_object()) return false;
	Json ignored;
	std::string error;

	if (schema.contains("entity")) {
		uint64_t raw = 0;
		if (!DecodeIntegral<uint64_t>(value, raw, ignored, error)) return false;
		entity = schema["entity"].get<std::string>();
		id = raw;
		return true;
	}

	if (!schema.contains("id_path") || !schema["id_path"].is_string()) return false;
	const std::string path = schema["id_path"].get<std::string>();
	if (!value.is_object() || !value.contains(path)) return false;
	uint64_t raw = 0;
	if (!DecodeIntegral<uint64_t>(value[path], raw, ignored, error)) return false;

	const Json &by = schema["entity_by"];
	const std::string field = by["field"].get<std::string>();
	if (!value.contains(field) || !value[field].is_string()) return false;
	const std::string key = value[field].get<std::string>();
	const Json &map = by["map"];
	if (!map.contains(key)) return false;

	entity = map[key].get<std::string>();
	id = raw;
	return true;
}

/**
 * Collect the entity one argument names.
 * @param value The normalized value of the argument.
 * @param index The position of the argument.
 * @param targets Receives the target, when the argument names an entity.
 */
template <typename T>
void CollectTarget(const Json &value, size_t index, Json &targets)
{
	const Json schema = TargetSchema<T>();
	std::string entity;
	uint64_t id = 0;
	if (!ResolveTarget(schema, value, entity, id)) return;

	targets.push_back({
		{"entity", entity},
		{"id", id},
		{"argument", index},
		{"via", schema.contains("id_path") && schema["id_path"].is_string() ? schema["id_path"].get<std::string>() : std::string("argument")},
	});
}

/**
 * Collect the entities all arguments of a command name.
 * @param normalized The decoded arguments.
 * @param targets Receives the targets.
 */
template <typename Ttuple, size_t... I>
void CollectTargets(const Json &normalized, Json &targets, std::index_sequence<I...>)
{
	(CollectTarget<std::tuple_element_t<I, Ttuple>>(normalized[I], I, targets), ...);
}

/**
 * Whether a list of {entity, id} pairs contains one pair.
 * @param pairs The list to search.
 * @param entity The entity kind.
 * @param id The id of the entity.
 * @return True when the pair is in the list.
 */
bool HasTarget(const Json &pairs, const std::string &entity, uint64_t id)
{
	for (const Json &have : pairs) {
		if (have["entity"].get<std::string>() != entity) continue;
		if (have["id"].get<uint64_t>() != id) continue;
		return true;
	}
	return false;
}

/**
 * Check that the arguments and the guarded refs name exactly the same entities.
 *
 * This is a guard, not an authority: the bridge decides which entities may be touched, and the
 * module proves that the typed arguments name exactly the guarded refs. It is a set equality in
 * both directions on purpose:
 *   - an argument entity missing from the guard is rejected, so a write cannot touch an object the
 *     bridge never resolved (the bridge only checks the refs it was given);
 *   - a guarded entity the arguments do not name is rejected, so an unrelated valid ref cannot
 *     authorize a reused target;
 *   - the actor company is always an argument entity, so it has to be guarded too.
 * @param expected The guarded entities: [{"entity": kind, "id": N}].
 * @param targets The entities the decoded arguments reference.
 * @param missing Receives the argument entities the guard does not name.
 * @param unexpected Receives the guarded entities the arguments do not name.
 * @param error Receives the reason when the guard does not hold.
 * @return True when both lists name the same entities.
 */
bool CheckExpectedTargets(const Json &expected, const Json &targets, Json &missing, Json &unexpected, std::string &error)
{
	if (!expected.is_array()) {
		error = "'expected_targets' must be an array of {\"entity\": kind, \"id\": N}";
		return false;
	}

	Json wanted = Json::array();
	for (const Json &want : expected) {
		if (!want.is_object() || !want.contains("entity") || !want["entity"].is_string() || !want.contains("id")) {
			error = "'expected_targets' entries are {\"entity\": kind, \"id\": N}";
			return false;
		}
		uint64_t id = 0;
		Json ignored;
		if (!DecodeIntegral<uint64_t>(want["id"], id, ignored, error)) return false;
		const std::string entity = want["entity"].get<std::string>();
		wanted.push_back({{"entity", entity}, {"id", id}});
		if (!HasTarget(targets, entity, id)) unexpected.push_back({{"entity", entity}, {"id", id}});
	}

	for (const Json &have : targets) {
		const std::string entity = have["entity"].get<std::string>();
		const uint64_t id = have["id"].get<uint64_t>();
		if (!HasTarget(wanted, entity, id) && !HasTarget(missing, entity, id)) {
			missing.push_back({{"entity", entity}, {"id", id}});
		}
	}

	if (missing.empty() && unexpected.empty()) return true;

	error = "expected_targets and the command arguments name different entities";
	if (!missing.empty()) error += "; not guarded: " + missing.dump();
	if (!unexpected.empty()) error += "; not named by the arguments: " + unexpected.dump();
	return false;
}

/** One dispatch call: what to run, what came back, and how it went. */
struct DispatchCall {
	const Json *args = nullptr; ///< Ordered arguments of the request.
	const Json *expected_targets = nullptr; ///< Entities the arguments must reference, or nullptr.
	TileIndex location{}; ///< Tile for commands with an implicit location argument.
	CompanyID company = CompanyID::Invalid(); ///< Company that pays and owns the result.
	bool dry_run = false; ///< Estimate only.
	CommandCost cost; ///< The actual result of the command.
	Json normalized = Json::array(); ///< The arguments as they were decoded.
	Json targets = Json::array(); ///< The entities the arguments reference, the actor company first.
	Json missing_targets = Json::array(); ///< Argument entities the guard does not name.
	Json unexpected_targets = Json::array(); ///< Guarded entities the arguments do not name.
	const char *error_code = "invalid_arguments"; ///< The error code when the call failed.
	std::string error; ///< The reason when the call failed.
};

/**
 * Add the actor company as a target.
 *
 * The company pays for and owns the result of every command, so it is an entity the write acts on
 * even when no argument names it.
 * @param company The company that runs the command.
 * @param targets Receives the company target, when a company is set.
 */
void CollectCompanyTarget(CompanyID company, Json &targets)
{
	const uint64_t id = static_cast<uint64_t>(company.base());
	if (id == static_cast<uint64_t>(CompanyID::Invalid().base())) return;
	targets.push_back({{"entity", "company"}, {"id", id}, {"via", "company_context"}});
}

/** The detail of a failed call; defined with the method handlers below. */
Json CallDetail(Commands cmd, const DispatchCall &call);

/**
 * The guard a call has to pass before anything is estimated or posted.
 *
 * Kept out of the per-command templates: the check is the same for every command.
 * @param call The call to check.
 * @return True when the guard holds.
 */
bool CheckCallTargets(DispatchCall &call)
{
	if (call.expected_targets == nullptr) {
		call.error = "world.command needs 'expected_targets': every {entity, id} the arguments name, including the actor company";
		call.error_code = "missing_expected_targets";
		return false;
	}
	if (!CheckExpectedTargets(*call.expected_targets, call.targets, call.missing_targets, call.unexpected_targets, call.error)) {
		call.error_code = "target_mismatch";
		return false;
	}
	return true;
}

/** Signature of the per-command argument decoder. */
using DecodeProc = bool (*)(DispatchCall &call);

/** Signature of the per-command dispatch function. */
using DispatchProc = bool (*)(DispatchCall &call);

/**
 * Decode the arguments of one command without running it.
 * @param call The call to decode the arguments of.
 * @return True when the arguments were decoded.
 */
template <Commands Tcmd>
bool DecodeTypedCommand(DispatchCall &call)
{
	using Args = typename CommandTraits<Tcmd>::Args;
	Args values{};
	if (!DecodeArgs(*call.args, values, call.normalized, call.error)) {
		call.error_code = "invalid_arguments";
		return false;
	}
	CollectCompanyTarget(call.company, call.targets);
	CollectTargets<Args>(call.normalized, call.targets, std::make_index_sequence<std::tuple_size_v<Args>>{});
	return call.expected_targets == nullptr || CheckCallTargets(call);
}

/** The argument decoder of a command, or nullptr when its arguments have no JSON decoding. */
template <Commands Tcmd>
constexpr DecodeProc MakeDecodeProc()
{
	if constexpr (ArgsDecodable<typename CommandTraits<Tcmd>::Args>::value) {
		return &DecodeTypedCommand<Tcmd>;
	} else {
		return nullptr;
	}
}

/**
 * Run one command from JSON arguments through the normal player command path.
 * @param call The call to run.
 * @return True when the command was posted (or estimated) and the engine reported a result.
 */
template <Commands Tcmd>
bool DispatchTypedCommand(DispatchCall &call)
{
	using Args = typename CommandTraits<Tcmd>::Args;
	Args values{};
	if (!DecodeArgs(*call.args, values, call.normalized, call.error)) {
		call.error_code = "invalid_arguments";
		return false;
	}

	/* The company is the implicit target of every command: it pays and owns the result. */
	CollectCompanyTarget(call.company, call.targets);
	CollectTargets<Args>(call.normalized, call.targets, std::make_index_sequence<std::tuple_size_v<Args>>{});
	/* The guard is mandatory for a write: the refs the arguments name have to be exactly the refs
	 * the bridge resolved, so neither an unguarded object nor an unrelated ref can slip through. */
	if (!CheckCallTargets(call)) return false;

	if (call.dry_run) {
		/* The engine's own cost query: no execution flag, so nothing is built or paid for. */
		call.cost = ExtractCommandCost(std::apply([](auto... arguments) {
			return Command<Tcmd>::Do(DoCommandFlag::QueryCost, arguments...);
		}, values));
		return true;
	}

	const TileIndex location = call.location;
	_posting = {};
	if constexpr (CommandTraits<Tcmd>::flags.Test(CommandFlag::Location)) {
		std::apply([location](auto... arguments) {
			Command<Tcmd>::Post((StringID)0, &AgentCommandCatalogCallback, location, arguments...);
		}, values);
	} else {
		std::apply([](auto... arguments) {
			Command<Tcmd>::Post((StringID)0, &AgentCommandCatalogCallback, arguments...);
		}, values);
	}
	if (!_posting.captured) {
		/* Post can refuse the command before the proc runs, e.g. when the game is paused for it. */
		call.error = "the engine did not run the command";
		call.error_code = "command_not_run";
		return false;
	}
	call.cost = _posting.cost;
	return true;
}

/** The dispatch entry of a command, or nullptr when its arguments have no JSON decoding. */
template <Commands Tcmd>
constexpr DispatchProc MakeDispatchProc()
{
	if constexpr (ArgsDecodable<typename CommandTraits<Tcmd>::Args>::value) {
		return &DispatchTypedCommand<Tcmd>;
	} else {
		return nullptr;
	}
}

/** The catalog and dispatch operations of one command. */
struct CommandOps {
	Json (*describe)(bool full); ///< Build the catalog entry of the command.
	DecodeProc decode; ///< Decode the arguments of the command, or nullptr when it is a gap.
	DispatchProc dispatch; ///< Run the command, or nullptr when it is a gap.
};

/** Build the per-command table from the engine's own command traits. */
template <typename T, T... i>
constexpr auto MakeCommandOps(std::integer_sequence<T, i...>)
{
	return EnumIndexArray<CommandOps, Commands, static_cast<Commands>(sizeof...(i))>{{{
		{ &DescribeCommand<static_cast<Commands>(i)>, MakeDecodeProc<static_cast<Commands>(i)>(), MakeDispatchProc<static_cast<Commands>(i)>() }...
	}}};
}

/** Catalog and dispatch operations of every engine command, indexed by `Commands`. */
constexpr auto _command_ops = MakeCommandOps(std::make_integer_sequence<std::underlying_type_t<Commands>, to_underlying(Commands::End)>{});

/* -- command description ----------------------------------------------------------------- */

/** @copydoc DescribeCommand */
template <Commands Tcmd>
Json DescribeCommand(bool full)
{
	using Traits = CommandTraits<Tcmd>;
	const bool supported = MakeDispatchProc<Tcmd>() != nullptr;

	Json entry = Json::object();
	entry["id"] = to_underlying(Tcmd);
	entry["name"] = std::string(Traits::name);
	entry["alias"] = CommandAlias(Traits::name);
	entry["type"] = std::string(CommandTypeName(Traits::type));
	entry["flags"] = FlagsJson(Traits::flags);
	entry["location_argument"] = Traits::flags.Test(CommandFlag::Location);
	entry["pause"] = {{"allowed_when_paused", IsCommandAllowedWhilePaused(Tcmd)}};
	entry["company"] = CompanyConstraints(Traits::flags);

	Json support = {{"supported", supported}, {"status", supported ? "dispatchable" : "gap"}};
	if (!supported) support["reason"] = UnsupportedArgsReason(static_cast<const typename Traits::Args *>(nullptr));
	entry["support"] = std::move(support);

	Json arguments = Json::array();
	Json targets = Json::array();
	DescribeArgs(static_cast<const typename Traits::Args *>(nullptr), arguments, targets, std::make_index_sequence<std::tuple_size_v<typename Traits::Args>>{});
	entry["targets"] = targets;
	/* Without an entity argument the only target of the command is the company that runs it. */
	entry["target_scope"] = targets.empty() ? "company" : "entities";
	/* Exactly the entities the arguments name, plus the actor company, and nothing else. */
	entry["expected_refs"] = ExpectedRefsMetadata(targets);

	if (full) {
		entry["arguments"] = std::move(arguments);

		using RetTypes = typename Traits::RetTypes;
		if constexpr (std::is_void_v<RetTypes>) {
			entry["returns"] = Json::array();
		} else {
			entry["returns"] = ReturnNames(static_cast<const RetTypes *>(nullptr));
		}
	}

	return entry;
}

/* -- name lookup ------------------------------------------------------------------------- */

/**
 * Lower case a name, so command names are matched case insensitively.
 * @param text The text to fold.
 * @return The folded text.
 */
std::string ToLower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

/**
 * The names the catalog accepts for a command, mapped onto the command.
 * @return Proc names and aliases, lower cased.
 */
const std::map<std::string, Commands> &CommandNames()
{
	static const std::map<std::string, Commands> names = [] {
		std::map<std::string, Commands> map;
		for (int i = 0; i < to_underlying(Commands::End); i++) {
			const Commands cmd = static_cast<Commands>(i);
			const std::string name(GetCommandName(cmd));
			map[ToLower(name)] = cmd;
			map[ToLower(CommandAlias(name))] = cmd;
		}
		return map;
	}();
	return names;
}

/**
 * Find the command a request names, by id, proc name or alias.
 * @param value The `command` field of the request.
 * @return The command, or nothing when the field does not name one.
 */
std::optional<Commands> FindCommand(const Json &value)
{
	if (value.is_number_unsigned() || value.is_number_integer()) {
		if (value.is_number_integer() && value.get<int64_t>() < 0) return std::nullopt;
		const uint64_t id = value.get<uint64_t>();
		if (id >= static_cast<uint64_t>(to_underlying(Commands::End))) return std::nullopt;
		return static_cast<Commands>(id);
	}
	if (value.is_string()) {
		const auto it = CommandNames().find(ToLower(value.get<std::string>()));
		if (it != CommandNames().end()) return it->second;
	}
	return std::nullopt;
}

/* -- results ----------------------------------------------------------------------------- */

/**
 * Build a failed tool result.
 * @param code The machine readable error code.
 * @param message Human readable message.
 * @param detail Optional structured detail.
 * @return Tool result with `ok` set to false.
 */
Json MakeError(std::string_view code, std::string_view message, Json detail = Json::object())
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = std::string(code);
	result["message"] = std::string(message);
	if (!detail.empty()) result["detail"] = std::move(detail);
	return result;
}

/**
 * Switch the company that pays and owns the result of a command, and restore the previous company
 * when the call returns.
 */
class CompanyContext {
public:
	explicit CompanyContext(CompanyID company) : backup(_current_company, company) {}

private:
	Backup<CompanyID> backup; ///< Restores the previous company on destruction.
};

/* -- methods ----------------------------------------------------------------------------- */

/**
 * Read the command catalog.
 * @param params The method parameters.
 * @return The method result.
 */
Json ReadCommandCatalog(const Json &params)
{
	std::string op = "list";
	if (params.is_object() && params.contains("op")) {
		if (!params["op"].is_string()) return MakeError("invalid_params", "'op' must be a string");
		op = params["op"].get<std::string>();
	}

	if (op != "list" && op != "describe" && op != "validate") {
		return MakeError("unsupported_op", "world.commands supports op=list, op=describe and op=validate", {{"op", op}});
	}

	if (op == "validate") {
		if (!params.contains("command")) return MakeError("invalid_params", "op=validate needs the engine command in 'command'");
		const std::optional<Commands> cmd = FindCommand(params["command"]);
		if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});

		Json args = Json::array();
		if (params.contains("args")) {
			if (!params["args"].is_array()) return MakeError("invalid_params", "'args' must be an array of the command's arguments");
			args = params["args"];
		}

		const DecodeProc decode = _command_ops[*cmd].decode;
		if (decode == nullptr) {
			return MakeError("unsupported_argument_types", "this command has arguments the generic decoder does not support yet", _command_ops[*cmd].describe(true)["support"]);
		}

		DispatchCall call;
		call.args = &args;
		/* A company makes the check exactly the one a write does: the actor company is a target. */
		if (params.contains("company")) {
			Json ignored;
			std::string error;
			if (!DecodeArg(params["company"], call.company, ignored, error)) {
				return MakeError("invalid_params", "'company' must be a company id: " + error);
			}
		}
		if (params.contains("expected_targets")) {
			if (!params["expected_targets"].is_array()) return MakeError("invalid_params", "'expected_targets' must be an array of {\"entity\": kind, \"id\": N}");
			call.expected_targets = &params["expected_targets"];
		}
		if (!decode(call)) {
			return MakeError(call.error_code, call.error, CallDetail(*cmd, call));
		}
		return {
			{"ok", true},
			{"op", op},
			{"command", std::string(GetCommandName(*cmd))},
			{"id", to_underlying(*cmd)},
			{"arguments", std::move(call.normalized)},
			{"targets", std::move(call.targets)},
		};
	}

	const bool full = op == "describe";
	const bool single = full && params.is_object() && params.contains("command");

	if (single) {
		const std::optional<Commands> cmd = FindCommand(params["command"]);
		if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});
		Json commands = Json::array();
		commands.push_back(_command_ops[*cmd].describe(true));
		return {
			{"ok", true},
			{"op", op},
			{"count", 1},
			{"commands", std::move(commands)},
		};
	}

	Json commands = Json::array();
	size_t dispatchable = 0;
	for (int i = 0; i < to_underlying(Commands::End); i++) {
		Json entry = _command_ops[static_cast<Commands>(i)].describe(full);
		if (entry["support"]["supported"].get<bool>()) dispatchable++;
		commands.push_back(std::move(entry));
	}

	return {
		{"ok", true},
		{"op", op},
		{"count", commands.size()},
		{"total", static_cast<size_t>(to_underlying(Commands::End))},
		{"dispatchable", dispatchable},
		{"commands", std::move(commands)},
	};
}

/**
 * The detail of a failed call: the command, the entities its arguments name and the guard diff.
 * @param cmd The command of the call.
 * @param call The failed call.
 * @return The error detail.
 */
Json CallDetail(Commands cmd, const DispatchCall &call)
{
	Json detail = {
		{"command", std::string(GetCommandName(cmd))},
		{"id", to_underlying(cmd)},
		{"targets", call.targets},
	};
	if (!call.missing_targets.empty()) detail["missing"] = call.missing_targets;
	if (!call.unexpected_targets.empty()) detail["unexpected"] = call.unexpected_targets;
	return detail;
}

/**
 * Run one engine command through the normal player command path.
 * @param params The method parameters.
 * @param company Company that pays and owns the result.
 * @return The method result.
 */
Json DispatchWorldCommand(const Json &params, CompanyID company)
{
	if (!params.is_object()) return MakeError("invalid_params", "world.command needs an object with 'command' and 'args'");
	if (!params.contains("command")) return MakeError("invalid_params", "world.command needs the engine command in 'command'");

	const std::optional<Commands> cmd = FindCommand(params["command"]);
	if (!cmd) return MakeError("unknown_command", "the request does not name an engine command", {{"command", params["command"]}});

	Json args = Json::array();
	if (params.contains("args")) {
		if (!params["args"].is_array()) return MakeError("invalid_params", "'args' must be an array of the command's arguments");
		args = params["args"];
	}

	bool dry_run = false;
	if (params.contains("dry_run")) {
		if (!params["dry_run"].is_boolean()) return MakeError("invalid_params", "'dry_run' must be a boolean");
		dry_run = params["dry_run"].get<bool>();
	}

	TileIndex location{};
	if (params.contains("location")) {
		uint32_t tile = 0;
		Json ignored;
		std::string error;
		if (!DecodeIntegral<uint32_t>(params["location"], tile, ignored, error)) {
			return MakeError("invalid_params", "'location' must be a tile index: " + error);
		}
		location = TileIndex{tile};
	}

	/* The entities the bridge has resolved and guarded; the arguments have to name exactly those. */
	const Json *expected_targets = nullptr;
	if (params.contains("expected_targets")) {
		if (!params["expected_targets"].is_array()) return MakeError("invalid_params", "'expected_targets' must be an array of {\"entity\": kind, \"id\": N}");
		expected_targets = &params["expected_targets"];
	}

	const DispatchProc dispatch = _command_ops[*cmd].dispatch;
	if (dispatch == nullptr) {
		return MakeError("unsupported_argument_types", "this command has arguments the generic decoder does not support yet", _command_ops[*cmd].describe(true)["support"]);
	}

	/* A write always has to declare the refs it acts on: the bridge only checks the refs it was
	 * given, so an absent list would leave the arguments unchecked. */
	if (expected_targets == nullptr) {
		return MakeError("missing_expected_targets", "world.command needs 'expected_targets': every {entity, id} the arguments name, including the actor company", {
			{"command", std::string(GetCommandName(*cmd))},
			{"id", to_underlying(*cmd)},
		});
	}

	/* The company pays and owns the result, so it has to exist before anything is estimated or run. */
	if (!Company::IsValidID(company)) return MakeError("invalid_company", "company does not exist", {{"company_id", company.base()}});

	/* Post is the player command path: in a network game it only queues the command for the server,
	 * so the engine's own answer would not come back. Reject instead of guessing. */
	if (!dry_run && _networking) return MakeError("multiplayer_unsupported", "catalog commands run through the single-player command path only");

	DispatchCall call;
	call.args = &args;
	call.expected_targets = expected_targets;
	call.location = location;
	call.company = company;
	call.dry_run = dry_run;
	{
		const CompanyContext context(company);
		if (!dispatch(call)) {
			return MakeError(call.error_code, call.error, CallDetail(*cmd, call));
		}
	}

	Json result = {
		{"ok", call.cost.Succeeded()},
		{"command", std::string(GetCommandName(*cmd))},
		{"id", to_underlying(*cmd)},
		{"dry_run", dry_run},
		{"cost", static_cast<int64_t>(call.cost.GetCost())},
		{"expense_type", to_underlying(call.cost.GetExpensesType())},
		{"arguments", std::move(call.normalized)},
		{"targets", std::move(call.targets)},
		{"guard", "checked"},
	};
	if (dry_run) {
		const CommandFlags flags = GetCommandFlags(*cmd);
		result["estimate_reliable"] = !flags.Test(CommandFlag::NoEst) && !flags.Test(CommandFlag::NoTest);
	}
	if (call.cost.Failed()) {
		result["error"] = "command_failed";
		result["message"] = "the engine rejected the command";
		result["message_id"] = call.cost.GetErrorMessage().base();
	}
	return result;
}

} // namespace

/**
 * Capture the actual cost of the command that was just posted.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param tile Location the command was posted for.
 */
void AgentCommandCatalogCallback(Commands, const CommandCost &result, TileIndex)
{
	_posting.captured = true;
	_posting.cost = result;
}

Json CommandCatalogCapabilities()
{
	/* Derive the counts and the undecodable types from the engine's own table, so the document
	 * cannot claim more than the dispatcher accepts. */
	size_t total = 0;
	size_t dispatchable = 0;
	std::vector<std::string> unsupported_types;
	std::vector<Json> gaps;

	for (int i = 0; i < to_underlying(Commands::End); i++) {
		const Json entry = _command_ops[static_cast<Commands>(i)].describe(true);
		total++;
		if (entry["support"]["supported"].get<bool>()) {
			dispatchable++;
			continue;
		}
		for (const Json &argument : entry["arguments"]) {
			if (argument["supported"].get<bool>()) continue;
			const std::string type = argument["type"].get<std::string>();
			if (std::ranges::find(unsupported_types, type) == unsupported_types.end()) unsupported_types.push_back(type);
		}
		gaps.push_back({
			{"command", entry["name"]},
			{"id", entry["id"]},
			{"reason", entry["support"]["reason"]},
		});
	}

	Json methods = Json::array();
	methods.push_back({
		{"method", "world.commands"},
		{"op", "list"},
		{"access", "read"},
		{"description", "every engine command with its id, name, flags, pause and company constraints, the entities its arguments name, and its support status"},
	});
	methods.push_back({
		{"method", "world.commands"},
		{"op", "describe"},
		{"access", "read"},
		{"description", "the same, plus the ordered argument schema, the JSON schema of each structured argument and the extra return values of one command or all of them"},
	});
	methods.push_back({
		{"method", "world.commands"},
		{"op", "validate"},
		{"access", "read"},
		{"description", "decode one command's arguments without running it, and report the arguments and the entities they name"},
	});
	methods.push_back({
		{"method", "world.command"},
		{"access", "write"},
		{"description", "post one engine command through the normal player command path, optionally as a cost estimate only"},
		{"params", {
			{"command", "engine command name, alias or numeric id"},
			{"args", "ordered positional arguments, as op=describe reports them"},
			{"dry_run", "estimate the cost only, never execute; estimate_reliable says whether the engine can estimate this command"},
			{"location", "tile for commands whose location_argument is true"},
			{"expected_targets", "[{\"entity\": kind, \"id\": N}]: the entities the bridge resolved; the typed arguments must name every one of them"},
		}},
	});

	Json argument_kinds = Json::array({
		"boolean", "integer", "enum", "enum_bit_set", "strong_id", "string",
		"vector", "array", "tuple", "encoded_string", "order", "source", "vehicle_list",
	});

	/* The entities an argument can name, and how the guard input is shaped. */
	Json target_kinds = Json::array({"town", "station", "vehicle", "industry", "group", "sign", "depot", "company", "engine"});

	Json gap_list = Json::array();
	gap_list.push_back({
		{"gap", "named_arguments"},
		{"detail", "arguments are positional; naming them needs a durable generator that emits the header names of every command"},
	});
	gap_list.push_back({
		{"gap", "enum_value_validation"},
		{"detail", "enums that declare an end marker (VehicleType, NewsType, GoalQuestionType) are checked against it and enum bit sets against their own mask; an enum without a marker is only range checked against its storage type, which each argument's schema.values states"},
	});
	gap_list.push_back({
		{"gap", "complex_argument_types"},
		{"detail", "commands whose arguments are engine structs or pointers are catalogued but not dispatchable"},
		{"types", unsupported_types},
		{"commands", gaps},
	});
	gap_list.push_back({
		{"gap", "special_return_values"},
		{"detail", "the generic callback reports the actual CommandCost only; created ids from procs that return more than a CommandCost are not reported"},
	});
	gap_list.push_back({
		{"gap", "blueprint_chunk_data"},
		{"detail", "BlueprintChunkData is an opaque byte blob the engine builds in PlanBlueprintPaste; the catalog refuses to accept a raw blob, so pasting a rail blueprint needs the rail blueprint project to produce the chunks"},
		{"commands", Json::array({"CmdPasteBlueprint", "CmdPrepareRailRoute"})},
	});
	gap_list.push_back({
		{"gap", "multiplayer"},
		{"detail", "writes use the single-player command path; a network game rejects them instead of queueing them"},
	});

	return {
		{"ok", true},
		{"methods", std::move(methods)},
		{"commands", {
			{"total", total},
			{"dispatchable", dispatchable},
			{"gaps", total - dispatchable},
			{"engine_end", static_cast<size_t>(to_underlying(Commands::End))},
		}},
		{"argument_kinds", std::move(argument_kinds)},
		{"targets", {
			{"kinds", std::move(target_kinds)},
			{"argument_metadata", "every argument carries \"target\": {\"entity\": kind, \"id_path\": null} when the value itself is the id, or {\"entity_by\": {\"field\": field, \"map\": {value: kind}}, \"id_path\": field} when the id sits inside a structured argument"},
			{"command_metadata", "every command carries \"targets\" (argument index + target descriptor), \"target_scope\": entities or company, and \"expected_refs\" with the guard contract"},
			{"guard", "expected_targets is required for world.command and must name exactly the {entity, id} pairs the typed arguments name plus the actor company; a missing pair, an extra pair or a different id is rejected with target_mismatch (detail.missing / detail.unexpected) before Post, and an absent list with missing_expected_targets"},
			{"authority", "the bridge resolves the refs and validates world, epoch and generation; the catalog only binds the actual argument ids to the guarded refs"},
		}},
		{"gaps", std::move(gap_list)},
	};
}

std::optional<Json> DispatchCommandCatalog(std::string_view method, const Json &params, CompanyID company)
{
	if (method == "world.commands") return ReadCommandCatalog(params);
	if (method == "world.command") return DispatchWorldCommand(params, company);
	return std::nullopt;
}

} // namespace InGameAgent
