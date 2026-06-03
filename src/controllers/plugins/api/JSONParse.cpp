// SPDX-FileCopyrightText: 2025 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/plugins/api/JSONParse.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/LuaUtilities.hpp"

#    include <boost/json/basic_parser.hpp>
#    include <boost/json/serialize.hpp>
#    include <QVarLengthArray>
#    include <sol/sol.hpp>

#    include <limits>

namespace {

using namespace chatterino::lua;

enum class ParseError : int {
    MoreThanOneTopLevel = 1,
    TooMuchNesting,
    NoElements,
    StackExhausted,
};

class ParseErrorCategory final : public boost::system::error_category
{
public:
    consteval ParseErrorCategory() = default;

    const char *name() const noexcept override
    {
        return "JSON parse error";
    }

    std::string message(int ev) const override
    {
        switch (static_cast<ParseError>(ev))
        {
            case ParseError::MoreThanOneTopLevel:
                return "There was more than one top level element";
            case ParseError::TooMuchNesting:
                return "Too much nesting";
            case ParseError::NoElements:
                return "Invalid state: no elements on stack";
            case ParseError::StackExhausted:
                return "Exhausted Lua stack (stack overflow)";
        }
        return "Unknown error code";
    }
};

constexpr ParseErrorCategory CATEGORY;

boost::system::error_code makeCode(ParseError kind)
{
    return {static_cast<int>(kind), CATEGORY};
}

[[nodiscard]] bool reserveStack(lua_State *state, int size,
                                boost::system::error_code &ec)
{
    if (lua_checkstack(state, size) == 0)
    {
        ec = makeCode(ParseError::StackExhausted);
        return false;
    }
    return true;
}

struct SaxHandler {
    constexpr static std::size_t max_object_size = 1 << 20;
    constexpr static std::size_t max_array_size = 1 << 28;
    constexpr static std::size_t max_key_size = 1 << 29;
    constexpr static std::size_t max_string_size = 1 << 30;

    constexpr static uint16_t MAX_NESTING = 256;

    SaxHandler(lua_State *state)
        : state(state)
    {
    }

    bool on_document_begin(boost::system::error_code &)
    {
        return true;
    }

    bool on_document_end(boost::system::error_code &)
    {
        return true;
    }

    bool on_object_begin(boost::system::error_code &ec)
    {
        this->elements.append(Element{
            .index = 0,
            .isObject = 1,
        });
        if (this->elements.size() > MAX_NESTING)
        {
            ec = makeCode(ParseError::TooMuchNesting);
            return false;
        }

        if (!reserveStack(this->state, 3, ec))
        {
            return false;
        }
        lua_newtable(this->state);

        return true;
    }

    bool on_object_end(size_t, boost::system::error_code &ec)
    {
        if (this->elements.empty())
        {
            ec = makeCode(ParseError::NoElements);
            assert(false && "No information about current on object on stack");
            return false;
        }
        assert(this->elements.back().isObject);
        this->elements.pop_back();

        return this->appendValue(ec);
    }

    bool on_array_begin(boost::system::error_code &ec)
    {
        this->elements.append(Element{
            .index = 0,
            .isObject = 0,
        });
        if (this->elements.size() > MAX_NESTING)
        {
            ec = makeCode(ParseError::TooMuchNesting);
            return false;
        }

        if (!reserveStack(this->state, 2, ec))
        {
            return false;
        }
        lua_newtable(this->state);

        return true;
    }

    bool on_array_end(size_t, boost::system::error_code &ec)
    {
        if (this->elements.empty())
        {
            ec = makeCode(ParseError::NoElements);
            assert(false && "No information about current array on stack");
            return false;
        }
        assert(!this->elements.back().isObject);
        this->elements.pop_back();

        return this->appendValue(ec);
    }

    bool on_key_part(boost::json::string_view sv, std::size_t,
                     boost::system::error_code &)
    {
        this->buffer.append(sv);
        return true;
    }

    bool on_key(boost::json::string_view sv, size_t,
                boost::system::error_code &)
    {
        if (this->buffer.empty())
        {
            lua_pushlstring(this->state, sv.data(), sv.size());
        }
        else
        {
            this->buffer.append(sv);
            lua_pushlstring(this->state, this->buffer.c_str(),
                            this->buffer.size());
            this->buffer.clear();
        }

        return true;
    }

    bool on_string_part(boost::json::string_view sv, size_t,
                        boost::system::error_code &)
    {
        this->buffer.append(sv);
        return true;
    }

    bool on_string(boost::json::string_view sv, size_t,
                   boost::system::error_code &ec)
    {
        if (this->buffer.empty())
        {
            lua_pushlstring(this->state, sv.data(), sv.size());
        }
        else
        {
            this->buffer.append(sv);
            lua_pushlstring(this->state, this->buffer.c_str(),
                            this->buffer.size());
            this->buffer.clear();
        }

        return this->appendValue(ec);
    }

    bool on_number_part(boost::json::string_view, boost::system::error_code &)
    {
        return true;
    }

    bool on_int64(int64_t i, boost::json::string_view,
                  boost::system::error_code &ec)
    {
        lua_pushinteger(this->state, static_cast<lua_Integer>(i));
        return this->appendValue(ec);
    }

    bool on_uint64(uint64_t i, boost::json::string_view,
                   boost::system::error_code &ec)
    {
        if (i <= static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max()))
        {
            lua_pushinteger(this->state, static_cast<lua_Integer>(i));
        }
        else
        {
            lua_pushnumber(this->state, static_cast<lua_Number>(i));
        }
        return this->appendValue(ec);
    }

    bool on_double(double d, boost::json::string_view,
                   boost::system::error_code &ec)
    {
        lua_pushnumber(this->state, d);
        return this->appendValue(ec);
    }

    bool on_bool(bool b, boost::system::error_code &ec)
    {
        lua_pushboolean(this->state, static_cast<int>(b));
        return this->appendValue(ec);
    }

    bool on_null(boost::system::error_code &ec)
    {
        lua_pushlightuserdata(this->state, nullptr);
        return this->appendValue(ec);
    }

    bool on_comment_part(boost::json::string_view, boost::system::error_code &)
    {
        return true;
    }

    bool on_comment(boost::json::string_view, boost::system::error_code &)
    {
        return true;
    }

private:
    bool appendValue(boost::system::error_code &ec)
    {
        if (this->elements.empty())
        {
            if (this->hadElement)
            {
                ec = makeCode(ParseError::MoreThanOneTopLevel);
                return false;
            }

            this->hadElement = true;
            return true;
        }

        if (this->elements.back().isObject)
        {
            lua_rawset(this->state, -3);
        }
        else
        {
            lua_rawseti(this->state, -2, ++this->elements.back().index);
        }

        return true;
    }

    struct Element {
        uint16_t index : 15;
        uint16_t isObject : 1;
    };

    lua_State *state = nullptr;

    QVarLengthArray<Element, 16> elements;

    bool hadElement = false;

    std::string buffer;

    friend class SaxParser;
};

class SaxParser
{
public:
    SaxParser(const boost::json::parse_options &opts, lua_State *state)
        : parser(opts, state)
    {
    }

    void parse(std::string_view sv)
    {
        boost::system::error_code ec;
        auto nRead = this->parser.write_some(false, sv.data(), sv.length(), ec);
        if (ec)
        {
            fail(this->parser.handler().state,
                 "Failed to parse JSON: %s (at %s) pos: %d",
                 ec.message().c_str(), ec.location().to_string().c_str(),
                 static_cast<int>(nRead));
        }
        if (nRead != sv.size())
        {
            fail(this->parser.handler().state,
                 "Failed to parse JSON: trailing junk", ec.message().c_str());
        }
    }

private:
    boost::json::basic_parser<SaxHandler> parser;
};

}  // namespace

namespace chatterino::lua::api {

int jsonParse(lua_State *L)
{
    auto nArgs = lua_gettop(L);
    luaL_argcheck(L, nArgs >= 1, 1, "expected at least one argument");

    boost::json::parse_options opts;
    opts.max_depth = SaxHandler::MAX_NESTING;
    opts.allow_invalid_utf8 = true;
#    if BOOST_VERSION >= 108700
    opts.allow_invalid_utf16 = true;
#    endif
    if (nArgs >= 2)
    {
        auto tbl = sol::stack::check_get<sol::table>(L, 2);
        if (tbl)
        {
            opts.allow_comments = tbl->get_or("allow_comments", false);
            opts.allow_trailing_commas =
                tbl->get_or("allow_trailing_commas", false);
        }
    }

    size_t size = 0;
    const char *str = luaL_checklstring(L, 1, &size);
    std::string_view sv(str, size);
    SaxParser parser(opts, L);
    parser.parse(sv);
    return 1;
}

}  // namespace chatterino::lua::api

#    include <boost/json/src.hpp>

#endif
