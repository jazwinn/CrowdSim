#pragma once

/// @file Reflect.hpp
/// Static reflection for plain data types: one descriptor per type lists its fields, and
/// JSON conversion, validation and the editor's property widgets are generated from it.
///
/// Describe a type once, at global namespace scope, beside the code that owns its format:
///
///     CROWDSIM_REFLECT(sim::Goal,
///         field("name", &T::name),
///         field("capacity", &T::capacity).tip("Agents served at once; 0 = unlimited"),
///         field("serviceTime", &T::serviceTime).atLeast(0).unit("s"),
///         check([](const T& g) { if (...) throw std::invalid_argument("..."); }))
///
/// - `field(key, &T::member)` reads and writes a member; `property(key, get, set)` reaches
///   anything else (a nested member, a value saved in other units, flags spread over tags).
///   `range(key, &T::lo, &T::hi)` saves two members as `[lo, hi]` (or one number for both).
/// - Field options: `label`, `tip`, `unit`, `atLeast`/`above`/`atMost`/`between`, `scale` (saved and shown
///   value = stored x scale; `degrees()` for radians), `color()` ("#rrggbb"),
///   `infinityAsNull()`, `lowercase()` (enum names), `flatten(omit)` (a nested type saved
///   inline), `widget(name)` (an editor-supplied
///   widget), `required()`, `hidden()` (saved, not shown) and `when(predicate)` (saved and shown only
///   when it holds; fields are read in order, so the predicate may use earlier fields).
/// - `check(fn)` runs after a type is read, for rules across fields; it throws
///   std::invalid_argument.
///
/// Values can be bool, integers, floats, std::string, enums (names via magic_enum),
/// glm::vec2/vec3, std::vector of strings or of described types, `Range<T>`, and described
/// types (nested objects). Specialise `Codec<V>` for anything else.
///
/// Reading (`fromJson`) merges: keys that are absent leave the value as it was, so the
/// defaults of a default-constructed value apply. Errors are `FieldError`s naming the path
/// to the field ("area.halfExtents: must be above 0").

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

namespace crowdsim::reflect {

using Json = nlohmann::json;

/// A float rounded to 6 significant digits, so saved files do not carry float noise.
[[nodiscard]] double roundFloat(float v);

/// "#rrggbb" from a 0xAABBGGRR colour, and back (alpha opaque).
[[nodiscard]] std::string colorString(std::uint32_t abgr);
[[nodiscard]] std::uint32_t colorFromString(const std::string& s);

/// Specialise (with CROWDSIM_REFLECT) to describe a type.
template <typename T>
struct Describe;

template <typename T>
concept Reflected = requires { Describe<T>::fields(); };

/// A field that failed to read or check, with the path to it.
class FieldError : public std::invalid_argument {
public:
    FieldError(std::string path, std::string reason)
        : std::invalid_argument(path.empty() ? reason : path + ": " + reason),
          path_(std::move(path)),
          reason_(std::move(reason)) {}
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

private:
    std::string path_;
    std::string reason_;
};

/// Two values saved as [lo, hi] (min/max ranges sampled per agent).
template <typename T>
struct Range {
    T lo{};
    T hi{};
    bool operator==(const Range&) const = default;
};

/// Everything a field says about itself besides how to reach it.
struct Meta {
    const char* label = nullptr;    ///< editor label (default: the key, humanised)
    const char* tooltip = nullptr;  ///< editor tooltip
    const char* unit = nullptr;     ///< shown after the value ("m", "s", "deg")
    const char* widget = nullptr;   ///< name of an editor-supplied widget for this field
    double min = -std::numeric_limits<double>::infinity();
    double max = std::numeric_limits<double>::infinity();
    bool minExclusive = false;
    double scale = 1.0;  ///< saved and shown value = stored value x scale
    double speed = 0.0;  ///< editor drag speed in shown units (0 = a sensible default)
    bool color = false;  ///< "#rrggbb" (uint32 ABGR or vec3 RGB in 0..1)
    bool infinityAsNull = false;
    bool lowercase = false;      ///< enum names saved in lower case
    bool hidden = false;         ///< saved, not shown in the editor
    bool required = false;       ///< reading fails when the key is absent
    bool flatten = false;        ///< a described type saved inline: its keys sit beside the owner's
    const char* omit = nullptr;  ///< with flatten: a key of the inner type left out

    [[nodiscard]] bool bounded() const noexcept { return std::isfinite(min) || std::isfinite(max); }
};

// --- Codecs: how a value type is saved, read and checked ------------------------------------

/// The range rule of a field as text ("must be above 0"), or empty when `v` (in shown units) is fine.
[[nodiscard]] std::string checkBounds(double shown, const Meta& m);

template <typename V, typename Enable = void>
struct Codec;  // undefined: an unsupported field type fails to compile here

template <typename T>
Json toJson(const T& value);
template <typename T>
void fromJson(const Json& j, T& value);

template <>
struct Codec<bool> {
    static Json encode(bool v, const Meta&) { return v; }
    static void decode(const Json& j, bool& v, const Meta&) {
        if (!j.is_boolean()) {
            throw FieldError({}, "expected true or false, got " + j.dump());
        }
        v = j.get<bool>();
    }
    static std::string check(bool, const Meta&) { return {}; }
};

template <typename V>
struct Codec<V, std::enable_if_t<std::is_integral_v<V> && !std::is_same_v<V, bool>>> {
    static Json encode(V v, const Meta& m) {
        if (m.color) {
            return colorString(static_cast<std::uint32_t>(v));
        }
        return v;
    }
    static void decode(const Json& j, V& v, const Meta& m) {
        if (m.color) {
            if (!j.is_string()) {
                throw FieldError({}, "expected a colour \"#rrggbb\", got " + j.dump());
            }
            v = static_cast<V>(colorFromString(j.get<std::string>()));
            return;
        }
        if (!j.is_number()) {
            throw FieldError({}, "expected a whole number, got " + j.dump());
        }
        const double d = j.get<double>();
        if (d != std::floor(d)) {
            throw FieldError({}, "expected a whole number, got " + j.dump());
        }
        if (d < static_cast<double>(std::numeric_limits<V>::lowest()) ||
            d > static_cast<double>(std::numeric_limits<V>::max())) {
            throw FieldError({}, "out of range: " + j.dump());
        }
        v = static_cast<V>(d);
    }
    static std::string check(V v, const Meta& m) {
        return m.color ? std::string() : checkBounds(static_cast<double>(v), m);
    }
};

template <typename V>
struct Codec<V, std::enable_if_t<std::is_floating_point_v<V>>> {
    static Json encode(V v, const Meta& m) {
        if (m.infinityAsNull && std::isinf(v)) {
            return nullptr;
        }
        return roundFloat(static_cast<float>(static_cast<double>(v) * m.scale));
    }
    static void decode(const Json& j, V& v, const Meta& m) {
        if (m.infinityAsNull && j.is_null()) {
            v = std::numeric_limits<V>::infinity();
            return;
        }
        if (!j.is_number()) {
            throw FieldError({}, "expected a number, got " + j.dump());
        }
        v = static_cast<V>(j.get<double>() / m.scale);
    }
    static std::string check(V v, const Meta& m) {
        if (m.infinityAsNull && std::isinf(v)) {
            return {};
        }
        return checkBounds(static_cast<double>(v) * m.scale, m);
    }
};

template <>
struct Codec<std::string> {
    static Json encode(const std::string& v, const Meta&) { return v; }
    static void decode(const Json& j, std::string& v, const Meta&) {
        if (!j.is_string()) {
            throw FieldError({}, "expected text, got " + j.dump());
        }
        v = j.get<std::string>();
    }
    static std::string check(const std::string&, const Meta&) { return {}; }
};

/// Lower-case copy of an enum name.
[[nodiscard]] std::string lower(std::string_view s);
/// Case-insensitive equality.
[[nodiscard]] bool sameName(std::string_view a, std::string_view b);

template <typename E>
struct Codec<E, std::enable_if_t<std::is_enum_v<E>>> {
    static Json encode(E v, const Meta& m) {
        const std::string_view name = magic_enum::enum_name(v);
        return m.lowercase ? lower(name) : std::string(name);
    }
    static void decode(const Json& j, E& v, const Meta& m) {
        if (j.is_string()) {
            for (const auto& [value, name] : magic_enum::enum_entries<E>()) {
                if (sameName(name, j.get<std::string>())) {
                    v = value;
                    return;
                }
            }
        }
        std::string names;
        for (const std::string_view name : magic_enum::enum_names<E>()) {
            names += (names.empty() ? "" : ", ") + (m.lowercase ? lower(name) : std::string(name));
        }
        throw FieldError({}, "expected one of " + names + ", got " + j.dump());
    }
    static std::string check(E, const Meta&) { return {}; }
};

template <glm::length_t N>
struct Codec<glm::vec<N, float, glm::defaultp>> {
    using V = glm::vec<N, float, glm::defaultp>;
    static Json encode(const V& v, const Meta& m) {
        if constexpr (N == 3) {
            if (m.color) {
                auto ch = [](float c) {
                    return static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
                };
                return colorString(0xff000000u | (ch(v.b) << 16) | (ch(v.g) << 8) | ch(v.r));
            }
        }
        Json a = Json::array();
        for (glm::length_t i = 0; i < N; ++i) {
            a.push_back(roundFloat(static_cast<float>(static_cast<double>(v[i]) * m.scale)));
        }
        return a;
    }
    static void decode(const Json& j, V& v, const Meta& m) {
        if constexpr (N == 3) {
            if (m.color) {
                if (!j.is_string()) {
                    throw FieldError({}, "expected a colour \"#rrggbb\", got " + j.dump());
                }
                const std::uint32_t c = colorFromString(j.get<std::string>());
                v = V(static_cast<float>(c & 0xffu) / 255.0f, static_cast<float>((c >> 8) & 0xffu) / 255.0f,
                      static_cast<float>((c >> 16) & 0xffu) / 255.0f);
                return;
            }
        }
        if (!j.is_array() || j.size() != static_cast<std::size_t>(N) ||
            !std::all_of(j.begin(), j.end(), [](const Json& x) { return x.is_number(); })) {
            throw FieldError({}, std::string(N == 2 ? "expected [x, z]" : "expected [x, y, z]") + ", got " + j.dump());
        }
        for (glm::length_t i = 0; i < N; ++i) {
            v[i] = static_cast<float>(j[static_cast<std::size_t>(i)].get<double>() / m.scale);
        }
    }
    static std::string check(const V& v, const Meta& m) {
        if (m.color) {
            return {};
        }
        for (glm::length_t i = 0; i < N; ++i) {
            if (std::string e = checkBounds(static_cast<double>(v[i]) * m.scale, m); !e.empty()) {
                return e;
            }
        }
        return {};
    }
};

template <typename T>
struct Codec<Range<T>> {
    static Json encode(const Range<T>& v, const Meta& m) {
        return Json::array({Codec<T>::encode(v.lo, m), Codec<T>::encode(v.hi, m)});
    }
    static void decode(const Json& j, Range<T>& v, const Meta& m) {
        if (j.is_number()) {  // one value for both ends
            Codec<T>::decode(j, v.lo, m);
            v.hi = v.lo;
            return;
        }
        if (!j.is_array() || j.size() != 2) {
            throw FieldError({}, "expected [min, max], got " + j.dump());
        }
        Codec<T>::decode(j[0], v.lo, m);
        Codec<T>::decode(j[1], v.hi, m);
    }
    static std::string check(const Range<T>& v, const Meta& m) {
        if (std::string e = Codec<T>::check(v.lo, m); !e.empty()) {
            return e;
        }
        if (std::string e = Codec<T>::check(v.hi, m); !e.empty()) {
            return e;
        }
        return v.hi < v.lo ? std::string("must be [min, max] with min <= max") : std::string();
    }
};

template <>
struct Codec<std::vector<std::string>> {
    static Json encode(const std::vector<std::string>& v, const Meta&) { return v; }
    static void decode(const Json& j, std::vector<std::string>& v, const Meta&) {
        if (!j.is_array() || !std::all_of(j.begin(), j.end(), [](const Json& x) { return x.is_string(); })) {
            throw FieldError({}, "expected a list of text, got " + j.dump());
        }
        v = j.get<std::vector<std::string>>();
    }
    static std::string check(const std::vector<std::string>&, const Meta&) { return {}; }
};

template <Reflected T>
struct Codec<T> {
    static Json encode(const T& v, const Meta&) { return toJson(v); }
    static void decode(const Json& j, T& v, const Meta&) { fromJson(j, v); }
    static std::string check(const T&, const Meta&) { return {}; }  // checked while reading
};

template <Reflected T>
struct Codec<std::vector<T>> {
    static Json encode(const std::vector<T>& v, const Meta&) {
        Json a = Json::array();
        for (const T& x : v) {
            a.push_back(toJson(x));
        }
        return a;
    }
    static void decode(const Json& j, std::vector<T>& v, const Meta&) {
        if (!j.is_array()) {
            throw FieldError({}, "expected a list, got " + j.dump());
        }
        std::vector<T> out(j.size());
        for (std::size_t i = 0; i < j.size(); ++i) {
            try {
                fromJson(j[i], out[i]);
            } catch (const FieldError& e) {
                throw FieldError(std::to_string(i) + (e.path().empty() ? "" : "." + e.path()), e.reason());
            }
        }
        v = std::move(out);
    }
    static std::string check(const std::vector<T>&, const Meta&) { return {}; }
};

// --- Fields -------------------------------------------------------------------------------

/// One described field of class `C` holding a `V`, reached through `get` and `set`.
template <typename C, typename V, typename Get, typename Set>
struct Property {
    using Class = C;
    using Value = V;

    const char* key;
    Get get;
    Set set;
    Meta meta{};
    bool (*predicate)(const C&) = nullptr;  ///< see when()

    [[nodiscard]] bool active(const C& c) const { return predicate == nullptr || predicate(c); }
    [[nodiscard]] const char* label() const { return meta.label != nullptr ? meta.label : key; }

    // Options (each returns a modified copy, so they chain).
#define CROWDSIM_REFLECT_OPTION(NAME, ARG, BODY) \
    [[nodiscard]] Property NAME(ARG) const {     \
        Property p = *this;                      \
        BODY;                                    \
        return p;                                \
    }
    CROWDSIM_REFLECT_OPTION(label, const char* text, p.meta.label = text)
    CROWDSIM_REFLECT_OPTION(tip, const char* text, p.meta.tooltip = text)
    CROWDSIM_REFLECT_OPTION(unit, const char* text, p.meta.unit = text)
    CROWDSIM_REFLECT_OPTION(widget, const char* name, p.meta.widget = name)
    CROWDSIM_REFLECT_OPTION(atLeast, double v, (p.meta.min = v, p.meta.minExclusive = false))
    CROWDSIM_REFLECT_OPTION(above, double v, (p.meta.min = v, p.meta.minExclusive = true))
    CROWDSIM_REFLECT_OPTION(atMost, double v, p.meta.max = v)
    CROWDSIM_REFLECT_OPTION(scale, double k, p.meta.scale = k)
    CROWDSIM_REFLECT_OPTION(speed, double s, p.meta.speed = s)
    CROWDSIM_REFLECT_OPTION(when, bool (*fn)(const C&), p.predicate = fn)
#undef CROWDSIM_REFLECT_OPTION
    [[nodiscard]] Property between(double lo, double hi) const { return atLeast(lo).atMost(hi); }
    [[nodiscard]] Property degrees() const { return scale(57.29577951308232).unit("deg"); }
    [[nodiscard]] Property color() const {
        Property p = *this;
        p.meta.color = true;
        return p;
    }
    [[nodiscard]] Property infinityAsNull() const {
        Property p = *this;
        p.meta.infinityAsNull = true;
        return p;
    }
    [[nodiscard]] Property lowercase() const {
        Property p = *this;
        p.meta.lowercase = true;
        return p;
    }
    /// Save a described member inline, its keys beside the owner's, optionally without `omitKey`.
    [[nodiscard]] Property flatten(const char* omitKey = nullptr) const {
        Property p = *this;
        p.meta.flatten = true;
        p.meta.omit = omitKey;
        return p;
    }
    [[nodiscard]] Property required() const {
        Property p = *this;
        p.meta.required = true;
        return p;
    }
    [[nodiscard]] Property hidden() const {
        Property p = *this;
        p.meta.hidden = true;
        return p;
    }
};

/// A rule across the fields of `C`, run after it is read.
template <typename C, typename Fn>
struct Check {
    using Class = C;
    Fn fn;
};

template <typename F>
inline constexpr bool isCheck = false;
template <typename C, typename Fn>
inline constexpr bool isCheck<Check<C, Fn>> = true;

/// A member field.
template <typename C, typename V>
[[nodiscard]] auto field(const char* key, V C::* member) {
    auto get = [member](const C& c) -> const V& { return c.*member; };
    auto set = [member](C& c, V v) { c.*member = std::move(v); };
    return Property<C, V, decltype(get), decltype(set)>{key, get, set};
}

/// A field reached through a getter and a setter (other units, nested members, ...).
template <typename C, typename Get, typename Set>
[[nodiscard]] auto property(const char* key, Get get, Set set) {
    using V = std::remove_cvref_t<std::invoke_result_t<Get, const C&>>;
    return Property<C, V, Get, Set>{key, get, set};
}

/// Two members saved together as [lo, hi].
template <typename C, typename V>
[[nodiscard]] auto range(const char* key, V C::* lo, V C::* hi) {
    auto get = [lo, hi](const C& c) { return Range<V>{c.*lo, c.*hi}; };
    auto set = [lo, hi](C& c, Range<V> r) {
        c.*lo = r.lo;
        c.*hi = r.hi;
    };
    return Property<C, Range<V>, decltype(get), decltype(set)>{key, get, set};
}

template <typename C, typename Fn>
[[nodiscard]] auto check(Fn fn) {
    return Check<C, Fn>{fn};
}

// --- Algorithms ---------------------------------------------------------------------------

/// Call `fn(field)` for each field of T, in order (checks are skipped).
template <Reflected T, typename Fn>
void forEachField(Fn&& fn) {
    std::apply(
        [&](const auto&... item) {
            (
                [&](const auto& f) {
                    if constexpr (!isCheck<std::remove_cvref_t<decltype(f)>>) {
                        fn(f);
                    }
                }(item),
                ...);
        },
        Describe<T>::fields());
}

template <typename T>
Json toJson(const T& value) {
    static_assert(Reflected<T>, "describe the type with CROWDSIM_REFLECT");
    Json j = Json::object();
    forEachField<T>([&](const auto& f) {
        if (f.active(value)) {
            using V = typename std::remove_cvref_t<decltype(f)>::Value;
            if constexpr (Reflected<V>) {
                if (f.meta.flatten) {
                    Json inner = toJson(f.get(value));
                    if (f.meta.omit != nullptr) {
                        inner.erase(f.meta.omit);
                    }
                    j.update(inner);
                    return;
                }
            }
            j[f.key] = Codec<V>::encode(f.get(value), f.meta);
        }
    });
    return j;
}

/// Read one field's JSON into `value` (decode and range check), without the field's path.
template <typename V>
void readValue(const Json& j, V& value, const Meta& meta) {
    Codec<V>::decode(j, value, meta);
    if (std::string e = Codec<V>::check(value, meta); !e.empty()) {
        throw FieldError({}, e);
    }
}

template <typename T>
void fromJson(const Json& j, T& value) {
    static_assert(Reflected<T>, "describe the type with CROWDSIM_REFLECT");
    if (!j.is_object()) {
        throw FieldError({}, "expected an object, got " + j.dump());
    }
    forEachField<T>([&](const auto& f) {
        if (!f.active(value)) {
            return;
        }
        using V = typename std::remove_cvref_t<decltype(f)>::Value;
        if constexpr (Reflected<V>) {
            if (f.meta.flatten) {
                Json inner = j;
                if (f.meta.omit != nullptr) {
                    inner.erase(f.meta.omit);
                }
                V v = f.get(value);
                fromJson(inner, v);
                f.set(value, std::move(v));
                return;
            }
        }
        const auto it = j.find(f.key);
        if (it == j.end()) {
            if (f.meta.required) {
                throw FieldError(f.key, "is required");
            }
            return;
        }
        V v = f.get(value);
        try {
            readValue(*it, v, f.meta);
        } catch (const FieldError& e) {
            throw FieldError(std::string(f.key) + (e.path().empty() ? "" : "." + e.path()), e.reason());
        } catch (const std::exception& e) {
            throw FieldError(f.key, e.what());
        }
        f.set(value, std::move(v));
    });
    std::apply(
        [&](const auto&... item) {
            (
                [&](const auto& c) {
                    if constexpr (isCheck<std::remove_cvref_t<decltype(c)>>) {
                        c.fn(value);
                    }
                }(item),
                ...);
        },
        Describe<T>::fields());
}

/// A value of T read from JSON over T's defaults.
template <Reflected T>
[[nodiscard]] T fromJson(const Json& j) {
    T value{};
    fromJson(j, value);
    return value;
}

}  // namespace crowdsim::reflect

/// Describe TYPE (at global namespace scope). Inside, `T` is TYPE: `field("key", &T::member)`,
/// `property<T>(...)`, `range("key", &T::lo, &T::hi)`, `check<T>(fn)`.
#define CROWDSIM_REFLECT(TYPE, ...)                                     \
    template <>                                                         \
    struct crowdsim::reflect::Describe<TYPE> {                          \
        using T = TYPE;                                                 \
        static const auto& fields() {                                   \
            using namespace crowdsim::reflect;                          \
            static const auto described = std::make_tuple(__VA_ARGS__); \
            return described;                                           \
        }                                                               \
    };
