#include "cvm/format.h"
#include "cvm/opcode.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

enum class TypeKind {
    Void,
    Character,
    Short,
    Integer,
    Floating,
    Long,
    LongLong,
    Float,
    Double,
    Pointer,
    FunctionPointer,
    Array,
    Structure,
    Union,
    Enumeration
};

struct CType;
using TypePtr = std::shared_ptr<CType>;

struct CField {
    std::string name;
    TypePtr type;
    std::uint32_t offset{};
};

struct CType {
    TypeKind kind{TypeKind::Void};
    bool isUnsigned{};
    std::uint32_t size{};
    std::uint32_t alignment{1};
    TypePtr element;
    std::uint32_t elementCount{};
    std::string tag;
    std::vector<CField> fields;
    TypePtr returnType;
    std::vector<TypePtr> parameterTypes;
    CvmCallingConvention callingConvention{CVM_CALL_CDECL};
    bool variadic{};
};

struct TargetDataModel {
    CvmTargetArch architecture{CVM_ARCH_X64};
    std::uint32_t pointerSize{8};
    std::uint32_t pointerAlignment{8};
};

TargetDataModel targetDataModel(CvmTargetArch architecture)
{
    if (architecture == CVM_ARCH_X86)
        return {CVM_ARCH_X86, 4, 4};
    if (architecture == CVM_ARCH_X64)
        return {CVM_ARCH_X64, 8, 8};
    throw std::runtime_error("unsupported target architecture");
}

TargetDataModel defaultTargetDataModel()
{
    return targetDataModel(
        sizeof(void *) == 4 ? CVM_ARCH_X86 : CVM_ARCH_X64);
}

TypePtr makeType(
    TypeKind kind,
    std::uint32_t size,
    std::uint32_t alignment,
    bool isUnsigned = false)
{
    auto type = std::make_shared<CType>();
    type->kind = kind;
    type->size = size;
    type->alignment = alignment;
    type->isUnsigned = isUnsigned;
    return type;
}

TypePtr voidType()
{
    static TypePtr value = makeType(TypeKind::Void, 0, 1);
    return value;
}

TypePtr intType()
{
    static TypePtr value = makeType(TypeKind::Integer, 4, 4);
    return value;
}

TypePtr charType()
{
    static TypePtr value = makeType(TypeKind::Character, 1, 1);
    return value;
}

TypePtr scalarType(TypeKind kind, bool isUnsigned)
{
    switch (kind) {
    case TypeKind::Character:
        return makeType(kind, 1, 1, isUnsigned);
    case TypeKind::Short:
        return makeType(kind, 2, 2, isUnsigned);
    case TypeKind::Integer:
    case TypeKind::Long:
        return makeType(kind, 4, 4, isUnsigned);
    case TypeKind::LongLong:
    case TypeKind::Double:
        return makeType(kind, 8, 8, isUnsigned);
    case TypeKind::Float:
        /* PicoC's single FP type is backed by a host double. */
        return makeType(kind, 8, 8, isUnsigned);
    default:
        return voidType();
    }
}

TypePtr pointerType(
    const TypePtr &element,
    const TargetDataModel &target)
{
    auto type = makeType(
        TypeKind::Pointer,
        target.pointerSize,
        target.pointerAlignment);
    type->element = element;
    return type;
}

TypePtr sizeType(const TargetDataModel &target)
{
    return target.pointerSize == 4
        ? scalarType(TypeKind::Integer, true)
        : scalarType(TypeKind::LongLong, true);
}

CvmValueType vmType(const TypePtr &type)
{
    switch (type->kind) {
    case TypeKind::Void: return CVM_TYPE_VOID;
    case TypeKind::Character:
        return type->isUnsigned ? CVM_TYPE_U8 : CVM_TYPE_I8;
    case TypeKind::Short:
        return type->isUnsigned ? CVM_TYPE_U16 : CVM_TYPE_I16;
    case TypeKind::Integer:
    case TypeKind::Long:
    case TypeKind::Enumeration:
        return type->isUnsigned ? CVM_TYPE_U32 : CVM_TYPE_I32;
    case TypeKind::LongLong:
        return type->isUnsigned ? CVM_TYPE_U64 : CVM_TYPE_I64;
    case TypeKind::Float: return CVM_TYPE_F64;
    case TypeKind::Double: return CVM_TYPE_F64;
    case TypeKind::Pointer:
    case TypeKind::FunctionPointer:
    case TypeKind::Array:
        return CVM_TYPE_PTR;
    default:
        return CVM_TYPE_VOID;
    }
}

bool isPointerLike(const TypePtr &type)
{
    return type->kind == TypeKind::Pointer ||
           type->kind == TypeKind::FunctionPointer ||
           type->kind == TypeKind::Array;
}

struct Macro {
    bool functionLike{};
    std::vector<std::string> parameters;
    std::string replacement;
};

class Preprocessor {
public:
    explicit Preprocessor(
        std::vector<std::filesystem::path> includePaths = {},
        CvmTargetArch architecture = CVM_ARCH_X64)
        : includePaths_(std::move(includePaths))
    {
        Macro platform;
        platform.replacement = "1";
        macros_.emplace(
            architecture == CVM_ARCH_X86 ? "_WIN32" : "_WIN64",
            platform);
    }

    std::string process(const std::filesystem::path &path)
    {
        return processFile(path, 0);
    }

private:
    struct Conditional {
        bool parentActive{};
        bool branchTaken{};
        bool active{};
    };

    std::unordered_map<std::string, Macro> macros_;
    std::vector<std::filesystem::path> includePaths_;
    bool inBlockComment_{};

    static std::string trim(const std::string &value)
    {
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return {};
        const auto last = value.find_last_not_of(" \t\r");
        return value.substr(first, last - first + 1);
    }

    static bool identifierStart(char value)
    {
        return std::isalpha(static_cast<unsigned char>(value)) ||
               value == '_';
    }

    static bool identifierPart(char value)
    {
        return std::isalnum(static_cast<unsigned char>(value)) ||
               value == '_';
    }

    bool active(const std::vector<Conditional> &conditions) const
    {
        return conditions.empty() || conditions.back().active;
    }

    class ConditionExpression {
    public:
        explicit ConditionExpression(std::string expression)
            : expression_(std::move(expression)) {}

        std::int64_t parse()
        {
            const std::int64_t value = logicalOr();
            skip();
            if (position_ != expression_.size())
                throw std::runtime_error(
                    "invalid preprocessor expression");
            return value;
        }

    private:
        std::string expression_;
        std::size_t position_{};

        void skip()
        {
            while (position_ < expression_.size() &&
                   std::isspace(static_cast<unsigned char>(
                       expression_[position_])))
                ++position_;
        }

        bool match(const char *text)
        {
            skip();
            const std::size_t length = std::strlen(text);
            if (expression_.compare(position_, length, text) != 0)
                return false;
            position_ += length;
            return true;
        }

        std::int64_t primary()
        {
            skip();
            if (match("(")) {
                const std::int64_t value = logicalOr();
                if (!match(")"))
                    throw std::runtime_error(
                        "missing ')' in preprocessor expression");
                return value;
            }
            if (position_ < expression_.size() &&
                identifierStart(expression_[position_])) {
                while (position_ < expression_.size() &&
                       identifierPart(expression_[position_]))
                    ++position_;
                return 0;
            }
            const char *start = expression_.c_str() + position_;
            char *end = nullptr;
            const std::int64_t value = std::strtoll(start, &end, 0);
            if (end == start)
                throw std::runtime_error(
                    "expected value in preprocessor expression");
            position_ += static_cast<std::size_t>(end - start);
            while (position_ < expression_.size() &&
                   std::strchr(
                       "uUlL",
                       expression_[position_]) != nullptr)
                ++position_;
            return value;
        }

        std::int64_t unary()
        {
            if (match("!"))
                return !unary();
            if (match("~"))
                return ~unary();
            if (match("+"))
                return unary();
            if (match("-"))
                return -unary();
            return primary();
        }

        std::int64_t multiply()
        {
            std::int64_t value = unary();
            for (;;) {
                if (match("*"))
                    value *= unary();
                else if (match("/")) {
                    const std::int64_t right = unary();
                    if (right == 0)
                        throw std::runtime_error(
                            "division by zero in #if");
                    value /= right;
                } else if (match("%")) {
                    const std::int64_t right = unary();
                    if (right == 0)
                        throw std::runtime_error(
                            "division by zero in #if");
                    value %= right;
                } else
                    return value;
            }
        }

        std::int64_t add()
        {
            std::int64_t value = multiply();
            for (;;) {
                if (match("+"))
                    value += multiply();
                else if (match("-"))
                    value -= multiply();
                else
                    return value;
            }
        }

        std::int64_t shift()
        {
            std::int64_t value = add();
            for (;;) {
                if (match("<<"))
                    value <<= add();
                else if (match(">>"))
                    value >>= add();
                else
                    return value;
            }
        }

        std::int64_t relational()
        {
            std::int64_t value = shift();
            for (;;) {
                if (match("<="))
                    value = value <= shift();
                else if (match(">="))
                    value = value >= shift();
                else if (match("<"))
                    value = value < shift();
                else if (match(">"))
                    value = value > shift();
                else
                    return value;
            }
        }

        std::int64_t equality()
        {
            std::int64_t value = relational();
            for (;;) {
                if (match("=="))
                    value = value == relational();
                else if (match("!="))
                    value = value != relational();
                else
                    return value;
            }
        }

        std::int64_t bitAnd()
        {
            std::int64_t value = equality();
            for (;;) {
                skip();
                if (expression_.compare(position_, 2, "&&") == 0)
                    return value;
                if (!match("&"))
                    return value;
                value &= equality();
            }
        }

        std::int64_t bitXor()
        {
            std::int64_t value = bitAnd();
            while (match("^"))
                value ^= bitAnd();
            return value;
        }

        std::int64_t bitOr()
        {
            std::int64_t value = bitXor();
            for (;;) {
                skip();
                if (expression_.compare(position_, 2, "||") == 0)
                    return value;
                if (!match("|"))
                    return value;
                value |= bitXor();
            }
        }

        std::int64_t logicalAnd()
        {
            std::int64_t value = bitOr();
            while (match("&&")) {
                const std::int64_t right = bitOr();
                value = value != 0 && right != 0;
            }
            return value;
        }

        std::int64_t logicalOr()
        {
            std::int64_t value = logicalAnd();
            while (match("||")) {
                const std::int64_t right = logicalAnd();
                value = value != 0 || right != 0;
            }
            return value;
        }
    };

    std::string replaceDefined(const std::string &input) const
    {
        std::string output;
        for (std::size_t position = 0; position < input.size();) {
            if (input.compare(position, 7, "defined") != 0 ||
                (position != 0 &&
                 identifierPart(input[position - 1])) ||
                (position + 7 < input.size() &&
                 identifierPart(input[position + 7]))) {
                output.push_back(input[position++]);
                continue;
            }
            position += 7;
            while (position < input.size() &&
                   std::isspace(static_cast<unsigned char>(
                       input[position])))
                ++position;
            const bool parenthesized =
                position < input.size() && input[position] == '(';
            if (parenthesized) {
                ++position;
                while (position < input.size() &&
                       std::isspace(static_cast<unsigned char>(
                           input[position])))
                    ++position;
            }
            const std::size_t start = position;
            while (position < input.size() &&
                   identifierPart(input[position]))
                ++position;
            const std::string name =
                input.substr(start, position - start);
            if (parenthesized) {
                while (position < input.size() &&
                       std::isspace(static_cast<unsigned char>(
                           input[position])))
                    ++position;
                if (position >= input.size() ||
                    input[position] != ')')
                    throw std::runtime_error(
                        "malformed defined operator");
                ++position;
            }
            output += macros_.count(name) != 0 ? "1" : "0";
        }
        return output;
    }

    bool evaluateCondition(std::string expression)
    {
        expression = trim(expand(replaceDefined(expression), 0));
        return ConditionExpression(std::move(expression)).parse() != 0;
    }

    std::vector<std::string> parseArguments(
        const std::string &text,
        std::size_t &position)
    {
        std::vector<std::string> arguments;
        std::string current;
        int depth = 1;
        char quote = '\0';
        ++position;
        for (; position < text.size(); ++position) {
            const char value = text[position];
            if (quote != '\0') {
                current.push_back(value);
                if (value == '\\' && position + 1 < text.size())
                    current.push_back(text[++position]);
                else if (value == quote)
                    quote = '\0';
                continue;
            }
            if (value == '"' || value == '\'') {
                quote = value;
                current.push_back(value);
            } else if (value == '(') {
                ++depth;
                current.push_back(value);
            } else if (value == ')') {
                --depth;
                if (depth == 0) {
                    if (!arguments.empty() || !trim(current).empty())
                        arguments.push_back(trim(current));
                    return arguments;
                }
                current.push_back(value);
            } else if (value == ',' && depth == 1) {
                arguments.push_back(trim(current));
                current.clear();
            } else {
                current.push_back(value);
            }
        }
        throw std::runtime_error("unterminated macro invocation");
    }

    std::string substitute(
        const Macro &macro,
        const std::vector<std::string> &arguments,
        int depth)
    {
        if (arguments.size() != macro.parameters.size())
            throw std::runtime_error("macro argument count mismatch");
        std::unordered_map<std::string, std::string> values;
        for (std::size_t index = 0; index < arguments.size(); ++index)
            values.emplace(
                macro.parameters[index],
                expand(arguments[index], depth + 1));

        std::string result;
        for (std::size_t position = 0;
             position < macro.replacement.size();) {
            if (!identifierStart(macro.replacement[position])) {
                result.push_back(macro.replacement[position++]);
                continue;
            }
            const std::size_t start = position++;
            while (position < macro.replacement.size() &&
                   identifierPart(macro.replacement[position])) {
                ++position;
            }
            const std::string name =
                macro.replacement.substr(start, position - start);
            const auto found = values.find(name);
            result += found == values.end() ? name : found->second;
        }
        return expand(result, depth + 1);
    }

    std::string expand(const std::string &text, int depth)
    {
        if (depth > 64)
            throw std::runtime_error("macro expansion depth exceeded");
        std::string output;
        char quote = '\0';
        for (std::size_t position = 0; position < text.size();) {
            const char value = text[position];
            if (inBlockComment_) {
                output.push_back(value);
                ++position;
                if (value == '*' && position < text.size() &&
                    text[position] == '/') {
                    output.push_back(text[position++]);
                    inBlockComment_ = false;
                }
                continue;
            }
            if (quote != '\0') {
                output.push_back(value);
                ++position;
                if (value == '\\' && position < text.size())
                    output.push_back(text[position++]);
                else if (value == quote)
                    quote = '\0';
                continue;
            }
            if (value == '/' && position + 1 < text.size() &&
                text[position + 1] == '/') {
                output.append(text.substr(position));
                break;
            }
            if (value == '/' && position + 1 < text.size() &&
                text[position + 1] == '*') {
                output.push_back(value);
                output.push_back(text[position + 1]);
                position += 2;
                inBlockComment_ = true;
                continue;
            }
            if (value == '"' || value == '\'') {
                quote = value;
                output.push_back(value);
                ++position;
                continue;
            }
            if (!identifierStart(value)) {
                output.push_back(value);
                ++position;
                continue;
            }

            const std::size_t start = position++;
            while (position < text.size() &&
                   identifierPart(text[position])) {
                ++position;
            }
            const std::string name =
                text.substr(start, position - start);
            const auto found = macros_.find(name);
            if (found == macros_.end()) {
                output += name;
                continue;
            }

            const Macro &macro = found->second;
            if (!macro.functionLike) {
                output += expand(macro.replacement, depth + 1);
                continue;
            }

            std::size_t invocation = position;
            while (invocation < text.size() &&
                   std::isspace(
                       static_cast<unsigned char>(text[invocation]))) {
                ++invocation;
            }
            if (invocation >= text.size() || text[invocation] != '(') {
                output += name;
                continue;
            }
            position = invocation;
            const auto arguments = parseArguments(text, position);
            ++position;
            output += substitute(macro, arguments, depth + 1);
        }
        return output;
    }

    void defineMacro(const std::string &definition)
    {
        std::size_t position = 0;
        while (position < definition.size() &&
               std::isspace(
                   static_cast<unsigned char>(definition[position]))) {
            ++position;
        }
        const std::size_t start = position;
        while (position < definition.size() &&
               identifierPart(definition[position])) {
            ++position;
        }
        if (start == position)
            throw std::runtime_error("invalid macro definition");
        const std::string name =
            definition.substr(start, position - start);
        Macro macro;
        if (position < definition.size() &&
            definition[position] == '(') {
            macro.functionLike = true;
            ++position;
            for (;;) {
                while (position < definition.size() &&
                       std::isspace(static_cast<unsigned char>(
                           definition[position]))) {
                    ++position;
                }
                if (position < definition.size() &&
                    definition[position] == ')') {
                    ++position;
                    break;
                }
                const std::size_t parameterStart = position;
                while (position < definition.size() &&
                       identifierPart(definition[position])) {
                    ++position;
                }
                macro.parameters.push_back(
                    definition.substr(
                        parameterStart,
                        position - parameterStart));
                while (position < definition.size() &&
                       std::isspace(static_cast<unsigned char>(
                           definition[position]))) {
                    ++position;
                }
                if (position < definition.size() &&
                    definition[position] == ',') {
                    ++position;
                    continue;
                }
                if (position >= definition.size() ||
                    definition[position] != ')') {
                    throw std::runtime_error(
                        "invalid function-like macro");
                }
                ++position;
                break;
            }
        }
        macro.replacement = trim(definition.substr(position));
        macros_[name] = std::move(macro);
    }

    std::string processFile(
        const std::filesystem::path &path,
        int includeDepth)
    {
        if (includeDepth > 32)
            throw std::runtime_error("include depth exceeded");
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error(
                "cannot open include file '" + path.string() + "'");
        std::ostringstream result;
        std::vector<Conditional> conditions;
        std::string line;
        while (std::getline(input, line)) {
            while (!line.empty() && line.back() == '\\') {
                line.pop_back();
                std::string continuation;
                if (!std::getline(input, continuation))
                    break;
                line += continuation;
            }
            const std::string stripped = trim(line);
            if (stripped.rfind("#!", 0) == 0)
                continue;
            if (stripped.empty() || stripped.front() != '#') {
                if (active(conditions))
                    result << expand(line, 0) << '\n';
                continue;
            }

            const std::string directive = trim(stripped.substr(1));
            const auto split = directive.find_first_of(" \t");
            const std::string command =
                split == std::string::npos
                    ? directive
                    : directive.substr(0, split);
            const std::string argument =
                split == std::string::npos
                    ? std::string()
                    : trim(directive.substr(split + 1));

            if (command == "if" || command == "ifdef" ||
                command == "ifndef") {
                const bool parent = active(conditions);
                bool condition = false;
                if (command == "if")
                    condition = evaluateCondition(argument);
                else
                    condition = macros_.count(argument) != 0;
                if (command == "ifndef")
                    condition = !condition;
                conditions.push_back(
                    Conditional{
                        parent,
                        condition,
                        parent && condition});
            } else if (command == "elif") {
                if (conditions.empty())
                    throw std::runtime_error("#elif without #if");
                auto &condition = conditions.back();
                const bool branch =
                    !condition.branchTaken &&
                    evaluateCondition(argument);
                condition.active =
                    condition.parentActive && branch;
                condition.branchTaken =
                    condition.branchTaken || branch;
            } else if (command == "else") {
                if (conditions.empty())
                    throw std::runtime_error("#else without #if");
                auto &condition = conditions.back();
                condition.active =
                    condition.parentActive && !condition.branchTaken;
                condition.branchTaken = true;
            } else if (command == "endif") {
                if (conditions.empty())
                    throw std::runtime_error("#endif without #if");
                conditions.pop_back();
            } else if (!active(conditions)) {
                continue;
            } else if (command == "define") {
                defineMacro(argument);
            } else if (command == "undef") {
                macros_.erase(argument);
            } else if (command == "include") {
                if (argument.size() < 2)
                    throw std::runtime_error("invalid #include");
                const bool quoted =
                    argument.front() == '"' && argument.back() == '"';
                const bool angled =
                    argument.front() == '<' && argument.back() == '>';
                if (!quoted && !angled)
                    throw std::runtime_error("invalid #include target");
                const std::filesystem::path includeName =
                    argument.substr(1, argument.size() - 2);
                std::vector<std::filesystem::path> candidates;
                if (quoted)
                    candidates.push_back(path.parent_path() / includeName);
                for (const auto &directory : includePaths_)
                    candidates.push_back(directory / includeName);
                bool found = false;
                for (const auto &candidate : candidates) {
                    if (!std::filesystem::exists(candidate))
                        continue;
                    result << processFile(candidate, includeDepth + 1);
                    found = true;
                    break;
                }
                const std::string builtIn = includeName.string();
                const bool compilerProvided =
                    builtIn == "stdio.h" ||
                     builtIn == "stdlib.h" ||
                     builtIn == "string.h" ||
                     builtIn == "math.h" ||
                     builtIn == "ctype.h" ||
                     builtIn == "stdbool.h" ||
                     builtIn == "stdint.h" ||
                     builtIn == "stddef.h" ||
                     builtIn == "limits.h";
                if (!found && !compilerProvided)
                    throw std::runtime_error(
                        "cannot resolve include file '" +
                        includeName.string() + "'");
            } else {
                throw std::runtime_error(
                    "unsupported preprocessor directive #" + command);
            }
        }
        if (!conditions.empty())
            throw std::runtime_error("unterminated preprocessor condition");
        return result.str();
    }
};

enum class TokenKind {
    End,
    Identifier,
    Integer,
    Floating,
    String,
    Character,
    KwInt,
    KwChar,
    KwShort,
    KwLong,
    KwSigned,
    KwUnsigned,
    KwFloat,
    KwDouble,
    KwStatic,
    KwStruct,
    KwUnion,
    KwEnum,
    KwTypedef,
    KwSizeof,
    KwVoid,
    KwReturn,
    KwIf,
    KwElse,
    KwFor,
    KwWhile,
    KwDo,
    KwBreak,
    KwContinue,
    KwSwitch,
    KwCase,
    KwDefault,
    KwGoto,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Dot,
    Arrow,
    Comma,
    Semicolon,
    Assign,
    AddAssign,
    SubtractAssign,
    MultiplyAssign,
    DivideAssign,
    ModuloAssign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
    BitAnd,
    BitOr,
    BitXor,
    LogicalNot,
    Question,
    Colon,
    BitNot,
    ShiftLeft,
    ShiftRight,
    Increment,
    Decrement,
    Plus,
    Minus,
    Star,
    Slash,
    Percent
    ,
    Ellipsis
};

struct Token {
    TokenKind kind{};
    std::string text;
    std::int64_t integer{};
    double floating{};
    std::uint32_t line{};
    std::uint32_t column{};
};

class Lexer {
public:
    Lexer(std::string source, std::string file)
        : source_(std::move(source)), file_(std::move(file)) {}

    std::vector<Token> scan()
    {
        std::vector<Token> result;
        for (;;) {
            skipTrivia();
            Token token = next();
            result.push_back(token);
            if (token.kind == TokenKind::End)
                return result;
        }
    }

private:
    std::string source_;
    std::string file_;
    std::size_t position_{};
    std::uint32_t line_{1};
    std::uint32_t column_{1};

    char peek(std::size_t lookahead = 0) const
    {
        const std::size_t at = position_ + lookahead;
        return at < source_.size() ? source_[at] : '\0';
    }

    char take()
    {
        const char value = peek();
        if (value == '\0')
            return value;
        ++position_;
        if (value == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return value;
    }

    [[noreturn]] void fail(const std::string &message) const
    {
        throw std::runtime_error(
            file_ + ":" + std::to_string(line_) + ":" +
            std::to_string(column_) + ": " + message);
    }

    void skipTrivia()
    {
        for (;;) {
            while (std::isspace(static_cast<unsigned char>(peek())))
                take();
            if (peek() == '/' && peek(1) == '/') {
                while (peek() != '\0' && take() != '\n') {}
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                take();
                take();
                while (!(peek() == '*' && peek(1) == '/')) {
                    if (peek() == '\0')
                        fail("unterminated block comment");
                    take();
                }
                take();
                take();
                continue;
            }
            if (peek() == '#') {
                while (peek() != '\0' && take() != '\n') {}
                continue;
            }
            return;
        }
    }

    Token next()
    {
        Token token;
        token.line = line_;
        token.column = column_;
        const char first = peek();
        if (first == '\0') {
            token.kind = TokenKind::End;
            return token;
        }
        if (std::isalpha(static_cast<unsigned char>(first)) || first == '_') {
            while (std::isalnum(static_cast<unsigned char>(peek())) ||
                   peek() == '_' || peek() == '$') {
                token.text.push_back(take());
            }
            if (token.text == "int")
                token.kind = TokenKind::KwInt;
            else if (token.text == "char")
                token.kind = TokenKind::KwChar;
            else if (token.text == "short")
                token.kind = TokenKind::KwShort;
            else if (token.text == "long")
                token.kind = TokenKind::KwLong;
            else if (token.text == "signed")
                token.kind = TokenKind::KwSigned;
            else if (token.text == "unsigned")
                token.kind = TokenKind::KwUnsigned;
            else if (token.text == "float")
                token.kind = TokenKind::KwFloat;
            else if (token.text == "double")
                token.kind = TokenKind::KwDouble;
            else if (token.text == "static")
                token.kind = TokenKind::KwStatic;
            else if (token.text == "struct")
                token.kind = TokenKind::KwStruct;
            else if (token.text == "union")
                token.kind = TokenKind::KwUnion;
            else if (token.text == "enum")
                token.kind = TokenKind::KwEnum;
            else if (token.text == "typedef")
                token.kind = TokenKind::KwTypedef;
            else if (token.text == "sizeof")
                token.kind = TokenKind::KwSizeof;
            else if (token.text == "void")
                token.kind = TokenKind::KwVoid;
            else if (token.text == "return")
                token.kind = TokenKind::KwReturn;
            else if (token.text == "if")
                token.kind = TokenKind::KwIf;
            else if (token.text == "else")
                token.kind = TokenKind::KwElse;
            else if (token.text == "for")
                token.kind = TokenKind::KwFor;
            else if (token.text == "while")
                token.kind = TokenKind::KwWhile;
            else if (token.text == "do")
                token.kind = TokenKind::KwDo;
            else if (token.text == "break")
                token.kind = TokenKind::KwBreak;
            else if (token.text == "continue")
                token.kind = TokenKind::KwContinue;
            else if (token.text == "switch")
                token.kind = TokenKind::KwSwitch;
            else if (token.text == "case")
                token.kind = TokenKind::KwCase;
            else if (token.text == "default")
                token.kind = TokenKind::KwDefault;
            else if (token.text == "goto")
                token.kind = TokenKind::KwGoto;
            else
                token.kind = TokenKind::Identifier;
            return token;
        }
        if (std::isdigit(static_cast<unsigned char>(first))) {
            std::string spelling;
            bool floating = false;
            while (std::isalnum(static_cast<unsigned char>(peek())) ||
                   peek() == '.' || peek() == '+' || peek() == '-') {
                if (peek() == '.')
                    floating = true;
                if ((peek() == '+' || peek() == '-') &&
                    !spelling.empty() &&
                    spelling.back() != 'e' &&
                    spelling.back() != 'E') {
                    break;
                }
                spelling.push_back(take());
            }
            const bool hexadecimal =
                spelling.size() > 2 && spelling[0] == '0' &&
                (spelling[1] == 'x' || spelling[1] == 'X');
            if (floating ||
                (!hexadecimal &&
                 spelling.find_first_of("eE") != std::string::npos)) {
                token.kind = TokenKind::Floating;
                token.text = spelling;
                token.floating = std::strtod(spelling.c_str(), nullptr);
            } else {
                token.kind = TokenKind::Integer;
                token.text = spelling;
                while (!spelling.empty() &&
                       strchr("uUlL", spelling.back()) != nullptr) {
                    spelling.pop_back();
                }
                if (spelling.size() > 2 && spelling[0] == '0' &&
                    (spelling[1] == 'b' || spelling[1] == 'B')) {
                    token.integer = static_cast<std::int64_t>(
                        std::strtoull(spelling.c_str() + 2, nullptr, 2));
                } else {
                    token.integer = static_cast<std::int64_t>(
                        std::strtoull(spelling.c_str(), nullptr, 0));
                }
            }
            return token;
        }
        if (first == '"' || first == '\'') {
            const char quote = take();
            token.kind =
                quote == '"' ? TokenKind::String : TokenKind::Character;
            while (peek() != quote) {
                if (peek() == '\0' || peek() == '\n')
                    fail("unterminated literal");
                char value = take();
                if (value == '\\') {
                    const char escaped = take();
                    switch (escaped) {
                    case 'n': value = '\n'; break;
                    case 'r': value = '\r'; break;
                    case 't': value = '\t'; break;
                    case '0': value = '\0'; break;
                    case '\\': value = '\\'; break;
                    case '\'': value = '\''; break;
                    case '"': value = '"'; break;
                    case 'x': {
                        int parsed = 0;
                        int digits = 0;
                        while (digits < 2 && std::isxdigit(
                                   static_cast<unsigned char>(peek()))) {
                            const char digit = take();
                            parsed = parsed * 16 +
                                (std::isdigit(
                                     static_cast<unsigned char>(digit))
                                    ? digit - '0'
                                    : std::tolower(
                                          static_cast<unsigned char>(digit)) -
                                          'a' + 10);
                            ++digits;
                        }
                        if (digits == 0)
                            fail("hex escape requires digits");
                        value = static_cast<char>(parsed);
                        break;
                    }
                    default:
                        if (escaped >= '0' && escaped <= '7') {
                            int parsed = escaped - '0';
                            int digits = 1;
                            while (digits < 3 && peek() >= '0' &&
                                   peek() <= '7') {
                                parsed = parsed * 8 + (take() - '0');
                                ++digits;
                            }
                            value = static_cast<char>(parsed);
                        } else {
                            fail("unsupported escape sequence");
                        }
                    }
                }
                token.text.push_back(value);
            }
            take();
            if (token.kind == TokenKind::Character) {
                if (token.text.size() != 1)
                    fail("character literal must contain one character");
                token.integer =
                    static_cast<unsigned char>(token.text.front());
            }
            return token;
        }

        const char second = peek(1);
        if (first == '.' && second == '.' && peek(2) == '.') {
            take(); take(); take();
            token.kind = TokenKind::Ellipsis;
            token.text = "...";
            return token;
        }
        if (first == '=' && second == '=') {
            take(); take(); token.kind = TokenKind::Equal; return token;
        }
        if (first == '!' && second == '=') {
            take(); take(); token.kind = TokenKind::NotEqual; return token;
        }
        if (first == '<' && second == '=') {
            take(); take(); token.kind = TokenKind::LessEqual; return token;
        }
        if (first == '>' && second == '=') {
            take(); take(); token.kind = TokenKind::GreaterEqual; return token;
        }
        if (first == '&' && second == '&') {
            take(); take(); token.kind = TokenKind::LogicalAnd; return token;
        }
        if (first == '|' && second == '|') {
            take(); take(); token.kind = TokenKind::LogicalOr; return token;
        }
        if (first == '<' && second == '<') {
            take(); take(); token.kind = TokenKind::ShiftLeft; return token;
        }
        if (first == '>' && second == '>') {
            take(); take(); token.kind = TokenKind::ShiftRight; return token;
        }
        if (first == '+' && second == '+') {
            take(); take(); token.kind = TokenKind::Increment; return token;
        }
        if (first == '+' && second == '=') {
            take(); take(); token.kind = TokenKind::AddAssign; return token;
        }
        if (first == '-' && second == '=') {
            take(); take(); token.kind = TokenKind::SubtractAssign; return token;
        }
        if (first == '*' && second == '=') {
            take(); take(); token.kind = TokenKind::MultiplyAssign; return token;
        }
        if (first == '/' && second == '=') {
            take(); take(); token.kind = TokenKind::DivideAssign; return token;
        }
        if (first == '%' && second == '=') {
            take(); take(); token.kind = TokenKind::ModuloAssign; return token;
        }
        if (first == '-' && second == '-') {
            take(); take(); token.kind = TokenKind::Decrement; return token;
        }
        if (first == '-' && second == '>') {
            take(); take(); token.kind = TokenKind::Arrow; return token;
        }

        take();
        switch (first) {
        case '(': token.kind = TokenKind::LeftParen; break;
        case ')': token.kind = TokenKind::RightParen; break;
        case '{': token.kind = TokenKind::LeftBrace; break;
        case '}': token.kind = TokenKind::RightBrace; break;
        case '[': token.kind = TokenKind::LeftBracket; break;
        case ']': token.kind = TokenKind::RightBracket; break;
        case '.': token.kind = TokenKind::Dot; break;
        case ',': token.kind = TokenKind::Comma; break;
        case ';': token.kind = TokenKind::Semicolon; break;
        case '=': token.kind = TokenKind::Assign; break;
        case '<': token.kind = TokenKind::Less; break;
        case '>': token.kind = TokenKind::Greater; break;
        case '&': token.kind = TokenKind::BitAnd; break;
        case '|': token.kind = TokenKind::BitOr; break;
        case '^': token.kind = TokenKind::BitXor; break;
        case '!': token.kind = TokenKind::LogicalNot; break;
        case '~': token.kind = TokenKind::BitNot; break;
        case '?': token.kind = TokenKind::Question; break;
        case ':': token.kind = TokenKind::Colon; break;
        case '+': token.kind = TokenKind::Plus; break;
        case '-': token.kind = TokenKind::Minus; break;
        case '*': token.kind = TokenKind::Star; break;
        case '/': token.kind = TokenKind::Slash; break;
        case '%': token.kind = TokenKind::Percent; break;
        default: fail(std::string("unexpected character '") + first + "'");
        }
        return token;
    }
};

struct Variable {
    std::uint32_t offset{};
    bool global{};
    TypePtr type{intType()};
};

struct CallFixup {
    std::uint32_t instruction{};
    std::string function;
    std::vector<TypePtr> argumentTypes;
    bool nativeDeclared{};
    bool nativeIndirect{};
    std::string library;
    std::string symbol;
    CvmCallingConvention callingConvention{CVM_CALL_CDECL};

    CallFixup() = default;
    CallFixup(
        std::uint32_t instructionValue,
        std::string functionValue,
        std::vector<TypePtr> argumentTypeValues)
        : instruction(instructionValue),
          function(std::move(functionValue)),
          argumentTypes(std::move(argumentTypeValues)) {}
};

struct Function {
    std::string name;
    TypePtr returnType{voidType()};
    std::vector<std::pair<std::string, CvmParameter>> parameters;
    std::unordered_map<std::string, Variable> variables;
    std::vector<CvmInstruction> code;
    std::vector<CallFixup> calls;
    std::uint32_t localBytes{};
    std::uint32_t localAlignment{1};
    bool hasReturn{};
    std::unordered_map<std::string, std::uint32_t> labels;
    std::vector<std::pair<std::uint32_t, std::string>> gotos;
};

struct FunctionPrototype {
    TypePtr returnType{intType()};
    std::vector<TypePtr> parameterTypes;
    bool variadic{};
    bool native{};
    std::string library;
    std::string symbol;
    CvmCallingConvention callingConvention{CVM_CALL_CDECL};

    FunctionPrototype() = default;
    FunctionPrototype(
        TypePtr returnTypeValue,
        std::vector<TypePtr> parameterTypeValues)
        : returnType(std::move(returnTypeValue)),
          parameterTypes(std::move(parameterTypeValues)) {}
};

struct CompilationUnit {
    std::vector<Function> functions;
    std::vector<std::uint8_t> data;
    std::uint32_t globalBytes{};
};

struct Expression {
    TypePtr type{voidType()};
    bool lvalue{};
};

struct LoopContext {
    std::vector<std::uint32_t> continues;
    std::vector<std::uint32_t> breaks;
    std::uint64_t nesting{};
};

struct SwitchContext {
    TypePtr type{intType()};
    std::uint32_t valueOffset{};
    std::vector<std::pair<std::int64_t, std::uint32_t>> cases;
    std::uint32_t defaultTarget{CVM_NO_INDEX};
    std::vector<std::uint32_t> breaks;
    std::uint64_t nesting{};
};

CvmInstruction makeInstruction(
    CvmOpcode opcode,
    CvmValueType type = CVM_TYPE_VOID,
    std::int32_t a = 0,
    std::int32_t b = 0)
{
    CvmInstruction instruction{};
    instruction.opcode = static_cast<std::uint8_t>(opcode);
    instruction.type = static_cast<std::uint8_t>(type);
    instruction.a = a;
    instruction.b = b;
    return instruction;
}

class Parser {
public:
    Parser(
        std::vector<Token> tokens,
        std::string file,
        TargetDataModel target)
        : target_(target),
          tokens_(std::move(tokens)),
          file_(std::move(file))
    {
        Function entry;
        entry.name = "__entry";
        entry.returnType = intType();
        functions_.push_back(std::move(entry));
        /*
         * The compatibility profile exposes the small, compiler-owned
         * header ABI. These are type/constant declarations only; behavior
         * remains an explicit PICOC import resolved by the host.
         */
        typedefs_.emplace("FILE", voidType());
        typedefs_.emplace("size_t", sizeType(target_));
        typedefs_.emplace(
            "wchar_t",
            scalarType(TypeKind::Short, true));
        enumConstants_.emplace("EOF", -1);
    }

    CompilationUnit parse()
    {
        function_ = &functions_[0];
        while (!at(TokenKind::End)) {
            if (at(TokenKind::Identifier) &&
                current().text.find('$') != std::string::npos &&
                position_ + 1 < tokens_.size() &&
                tokens_[position_ + 1].kind == TokenKind::Colon) {
                parseDfrDeclaration();
            } else if (at(TokenKind::KwTypedef)) {
                parseTypedef();
            } else if (at(TokenKind::Identifier) &&
                       typedefs_.count(current().text) == 0 &&
                       position_ + 1 < tokens_.size() &&
                       tokens_[position_ + 1].kind ==
                           TokenKind::LeftParen) {
                std::size_t lookahead = position_ + 2;
                int depth = 1;
                while (lookahead < tokens_.size() && depth != 0) {
                    if (tokens_[lookahead].kind ==
                        TokenKind::LeftParen)
                        ++depth;
                    else if (tokens_[lookahead].kind ==
                             TokenKind::RightParen)
                        --depth;
                    ++lookahead;
                }
                if (depth == 0 &&
                    lookahead < tokens_.size() &&
                    tokens_[lookahead].kind == TokenKind::LeftBrace) {
                    parseFunction();
                    function_ = &functions_[0];
                } else {
                    parseStatement();
                }
            } else if (isTypeStart(current().kind)) {
                const std::size_t saved = position_;
                (void)parseType();
                if (at(TokenKind::Semicolon)) {
                    position_ = saved;
                    parseGlobalDeclaration();
                    continue;
                }
                if (at(TokenKind::LeftParen)) {
                    position_ = saved;
                    parseGlobalDeclaration();
                    continue;
                }
                while (accept(TokenKind::Star)) {}
                (void)take(TokenKind::Identifier, "declaration name");
                const bool isFunction = at(TokenKind::LeftParen);
                position_ = saved;
                if (isFunction) {
                    parseFunction();
                    function_ = &functions_[0];
                } else {
                    parseGlobalDeclaration();
                }
            } else {
                parseStatement();
            }
        }
        resolveGotos(*function_);
        function_ = &functions_[0];
        for (const Function &candidate : functions_) {
            if (candidate.name == "main" &&
                (candidate.parameters.empty() ||
                 candidate.parameters.size() == 2)) {
                std::vector<TypePtr> mainArguments;
                if (candidate.parameters.size() == 2) {
                    function_->calls.push_back(CallFixup{
                        static_cast<std::uint32_t>(
                            function_->code.size()),
                        "__picoc_argc",
                        {}});
                    emit(makeInstruction(
                        CVM_OP_CALL,
                        CVM_TYPE_I32,
                        0,
                        0));
                    TypePtr argvType = pointerType(
                        pointerType(charType(), target_),
                        target_);
                    function_->calls.push_back(CallFixup{
                        static_cast<std::uint32_t>(
                            function_->code.size()),
                        "__picoc_argv",
                        {}});
                    emit(makeInstruction(
                        CVM_OP_CALL,
                        CVM_TYPE_PTR,
                        0,
                        0));
                    mainArguments = {intType(), argvType};
                }
                function_->calls.push_back(
                    CallFixup{
                        static_cast<std::uint32_t>(function_->code.size()),
                        "main",
                        mainArguments});
                emit(makeInstruction(
                    CVM_OP_CALL,
                    vmType(candidate.returnType),
                    0,
                    static_cast<std::int32_t>(mainArguments.size())));
                if (candidate.returnType->kind == TypeKind::Integer) {
                    emit(makeInstruction(CVM_OP_RETURN, CVM_TYPE_I32));
                    function_->hasReturn = true;
                }
                break;
            }
        }
        if (!function_->hasReturn) {
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                CVM_TYPE_I32,
                0));
            emit(makeInstruction(CVM_OP_RETURN, CVM_TYPE_I32));
        }
        CompilationUnit unit;
        unit.functions = std::move(functions_);
        unit.data = std::move(data_);
        unit.globalBytes = globalBytes_;
        return unit;
    }

private:
    TargetDataModel target_;
    std::vector<Token> tokens_;
    std::string file_;
    std::size_t position_{};
    std::vector<Function> functions_;
    std::unordered_map<std::string, Variable> globals_;
    std::unordered_map<std::string, TypePtr> taggedTypes_;
    std::unordered_map<std::string, TypePtr> typedefs_;
    std::unordered_map<std::string, std::int64_t> enumConstants_;
    std::unordered_map<std::string, FunctionPrototype> prototypes_;
    std::vector<std::uint8_t> data_;
    std::uint32_t globalBytes_{};
    Function *function_{};
    std::vector<LoopContext> loops_;
    std::vector<SwitchContext> switches_;
    std::uint64_t statementNesting_{};
    std::vector<std::unordered_map<std::string, Variable>> localScopes_{
        std::unordered_map<std::string, Variable>{}};

    const Token &current() const { return tokens_[position_]; }
    bool at(TokenKind kind) const { return current().kind == kind; }

    bool isTypeStart(TokenKind kind) const
    {
        return kind == TokenKind::KwVoid ||
               kind == TokenKind::KwChar ||
               kind == TokenKind::KwShort ||
               kind == TokenKind::KwInt ||
               kind == TokenKind::KwLong ||
               kind == TokenKind::KwSigned ||
               kind == TokenKind::KwUnsigned ||
               kind == TokenKind::KwFloat ||
               kind == TokenKind::KwDouble ||
               kind == TokenKind::KwStatic ||
               kind == TokenKind::KwStruct ||
               kind == TokenKind::KwUnion ||
               kind == TokenKind::KwEnum ||
               (kind == TokenKind::Identifier &&
                typedefs_.count(current().text) != 0);
    }

    [[noreturn]] void fail(const std::string &message) const
    {
        throw std::runtime_error(
            file_ + ":" + std::to_string(current().line) + ":" +
            std::to_string(current().column) + ": " + message);
    }

    Token take(TokenKind kind, const char *description)
    {
        if (!at(kind))
            fail(
                std::string("expected ") + description +
                ", got token '" + current().text + "' (kind " +
                std::to_string(
                    static_cast<unsigned>(current().kind)) + ")");
        return tokens_[position_++];
    }

    bool accept(TokenKind kind)
    {
        if (!at(kind))
            return false;
        ++position_;
        return true;
    }

    TypePtr parseFfiAtom()
    {
        if (accept(TokenKind::KwVoid))
            return voidType();
        const std::string atom =
            take(TokenKind::Identifier, "FFI type atom").text;
        if (atom == "i16")
            return scalarType(TypeKind::Short, false);
        if (atom == "u16")
            return scalarType(TypeKind::Short, true);
        if (atom == "i32")
            return scalarType(TypeKind::Integer, false);
        if (atom == "u32")
            return scalarType(TypeKind::Integer, true);
        if (atom == "i64")
            return scalarType(TypeKind::LongLong, false);
        if (atom == "u64")
            return scalarType(TypeKind::LongLong, true);
        if (atom == "ptr")
            return pointerType(voidType(), target_);
        if (atom == "cstr")
            return pointerType(charType(), target_);
        if (atom == "size_t")
            return sizeType(target_);
        fail("unsupported FFI type atom '" + atom + "'");
    }

    void registerNativePrototype(
        const std::string &name,
        const FunctionPrototype &prototype)
    {
        const auto found = prototypes_.find(name);
        if (found != prototypes_.end() &&
            (!found->second.native ||
             found->second.library != prototype.library ||
             found->second.symbol != prototype.symbol)) {
            fail("ambiguous native function name '" + name + "'");
        }
        prototypes_[name] = prototype;
    }

    void parseDfrDeclaration()
    {
        const std::string qualified =
            take(TokenKind::Identifier, "DFR function name").text;
        const std::size_t separator = qualified.find('$');
        if (separator == 0 || separator + 1 >= qualified.size() ||
            qualified.find('$', separator + 1) != std::string::npos) {
            fail("DFR name must use LIBRARY$Function");
        }
        take(TokenKind::Colon, "':'");

        CvmCallingConvention convention =
            target_.architecture == CVM_ARCH_X64
                ? CVM_CALL_WIN64
                : CVM_CALL_STDCALL;
        bool conventionExplicit = false;
        if (at(TokenKind::Identifier) &&
            (current().text == "cdecl" ||
             current().text == "stdcall")) {
            conventionExplicit = true;
            convention =
                current().text == "cdecl"
                    ? CVM_CALL_CDECL
                    : CVM_CALL_STDCALL;
            ++position_;
        }

        FunctionPrototype prototype;
        prototype.returnType = parseFfiAtom();
        prototype.native = true;
        prototype.library = qualified.substr(0, separator);
        prototype.symbol = qualified.substr(separator + 1);
        prototype.callingConvention = convention;
        take(TokenKind::LeftParen, "'('");
        if (!at(TokenKind::RightParen)) {
            for (;;) {
                if (accept(TokenKind::Ellipsis)) {
                    prototype.variadic = true;
                    if (convention == CVM_CALL_STDCALL &&
                        conventionExplicit)
                        fail("stdcall native function cannot be variadic");
                    if (convention == CVM_CALL_STDCALL)
                        convention = CVM_CALL_CDECL;
                    prototype.callingConvention = convention;
                    break;
                }
                TypePtr parameter = parseFfiAtom();
                if (parameter->kind == TypeKind::Void)
                    fail("native parameter cannot have void type");
                prototype.parameterTypes.push_back(parameter);
                if (!accept(TokenKind::Comma))
                    break;
            }
        }
        take(TokenKind::RightParen, "')'");
        take(TokenKind::Semicolon, "';'");

        registerNativePrototype(qualified, prototype);
        registerNativePrototype(prototype.symbol, prototype);
    }

    TypePtr parseType()
    {
        (void)accept(TokenKind::KwStatic);
        if (at(TokenKind::Identifier)) {
            const auto found = typedefs_.find(current().text);
            if (found != typedefs_.end()) {
                ++position_;
                return found->second;
            }
        }
        if (accept(TokenKind::KwStruct))
            return parseAggregate(TypeKind::Structure);
        if (accept(TokenKind::KwUnion))
            return parseAggregate(TypeKind::Union);
        if (accept(TokenKind::KwEnum))
            return parseEnumeration();
        if (accept(TokenKind::KwVoid))
            return voidType();
        bool isUnsigned = false;
        if (accept(TokenKind::KwUnsigned))
            isUnsigned = true;
        else
            (void)accept(TokenKind::KwSigned);
        if (accept(TokenKind::KwChar))
            return scalarType(TypeKind::Character, isUnsigned);
        if (accept(TokenKind::KwShort)) {
            (void)accept(TokenKind::KwInt);
            return scalarType(TypeKind::Short, isUnsigned);
        }
        if (accept(TokenKind::KwLong)) {
            if (accept(TokenKind::KwLong)) {
                (void)accept(TokenKind::KwInt);
                return scalarType(TypeKind::LongLong, isUnsigned);
            }
            (void)accept(TokenKind::KwInt);
            return scalarType(TypeKind::Long, isUnsigned);
        }
        if (accept(TokenKind::KwFloat))
            return scalarType(TypeKind::Float, false);
        if (accept(TokenKind::KwDouble))
            return scalarType(TypeKind::Double, false);
        if (accept(TokenKind::KwInt) || isUnsigned)
            return scalarType(TypeKind::Integer, isUnsigned);
        fail(
            "expected a type, got token '" + current().text +
            "' (kind " +
            std::to_string(static_cast<unsigned>(current().kind)) + ")");
    }

    TypePtr parseAggregate(TypeKind kind)
    {
        std::string tag;
        if (at(TokenKind::Identifier))
            tag = take(TokenKind::Identifier, "aggregate tag").text;

        TypePtr type;
        if (!tag.empty()) {
            const std::string key =
                (kind == TypeKind::Structure ? "struct:" : "union:") + tag;
            const auto found = taggedTypes_.find(key);
            if (found != taggedTypes_.end()) {
                type = found->second;
            } else {
                type = makeType(kind, 0, 1);
                type->tag = tag;
                taggedTypes_[key] = type;
            }
        } else {
            type = makeType(kind, 0, 1);
        }

        if (!accept(TokenKind::LeftBrace))
            return type;

        type->fields.clear();
        type->size = 0;
        type->alignment = 1;
        while (!at(TokenKind::RightBrace)) {
            TypePtr base = parseType();
            if (accept(TokenKind::Semicolon)) {
                if (base->kind != TypeKind::Structure &&
                    base->kind != TypeKind::Union) {
                    fail("anonymous field must be a struct or union");
                }
                type->alignment =
                    std::max(type->alignment, base->alignment);
                std::uint32_t embeddedOffset = 0;
                if (kind == TypeKind::Structure) {
                    type->size =
                        alignStorage(type->size, base->alignment);
                    embeddedOffset = type->size;
                    type->size += base->size;
                } else {
                    type->size = std::max(type->size, base->size);
                }
                for (const CField &nested : base->fields) {
                    CField promoted = nested;
                    promoted.offset += embeddedOffset;
                    type->fields.push_back(std::move(promoted));
                }
                continue;
            }
            for (;;) {
                TypePtr fieldType = parsePointerSuffix(base);
                const std::string name =
                    take(TokenKind::Identifier, "field name").text;
                fieldType = parseArraySuffix(fieldType);
                CField field;
                field.name = name;
                field.type = fieldType;
                type->alignment =
                    std::max(type->alignment, fieldType->alignment);
                if (kind == TypeKind::Structure) {
                    type->size =
                        alignStorage(type->size, fieldType->alignment);
                    field.offset = type->size;
                    type->size += fieldType->size;
                } else {
                    field.offset = 0;
                    type->size = std::max(type->size, fieldType->size);
                }
                type->fields.push_back(std::move(field));
                if (!accept(TokenKind::Comma))
                    break;
            }
            take(TokenKind::Semicolon, "';'");
        }
        take(TokenKind::RightBrace, "'}'");
        type->size = alignStorage(type->size, type->alignment);
        return type;
    }

    TypePtr parseEnumeration()
    {
        std::string tag;
        if (at(TokenKind::Identifier))
            tag = take(TokenKind::Identifier, "enum tag").text;
        auto type = makeType(TypeKind::Enumeration, 4, 4);
        type->tag = tag;
        if (!tag.empty()) {
            const auto found = taggedTypes_.find("enum:" + tag);
            if (found != taggedTypes_.end())
                type = found->second;
            else
                taggedTypes_["enum:" + tag] = type;
        }
        if (!accept(TokenKind::LeftBrace))
            return type;
        std::int64_t value = 0;
        while (!at(TokenKind::RightBrace)) {
            const std::string name =
                take(TokenKind::Identifier, "enumerator").text;
            if (accept(TokenKind::Assign)) {
                if (at(TokenKind::Integer))
                    value = take(TokenKind::Integer, "enum value").integer;
                else {
                    const auto found =
                        enumConstants_.find(
                            take(TokenKind::Identifier, "enum value").text);
                    if (found == enumConstants_.end())
                        fail("invalid enum constant expression");
                    value = found->second;
                }
            }
            enumConstants_[name] = value++;
            if (!accept(TokenKind::Comma))
                break;
        }
        take(TokenKind::RightBrace, "'}'");
        return type;
    }

    void parseTypedef()
    {
        take(TokenKind::KwTypedef, "'typedef'");
        TypePtr base = parseType();
        for (;;) {
            TypePtr type = parsePointerSuffix(base);
            const std::string name =
                take(TokenKind::Identifier, "typedef name").text;
            type = parseArraySuffix(type);
            typedefs_[name] = type;
            if (!accept(TokenKind::Comma))
                break;
        }
        take(TokenKind::Semicolon, "';'");
    }

    TypePtr parsePointerSuffix(TypePtr base)
    {
        while (accept(TokenKind::Star))
            base = pointerType(base, target_);
        return base;
    }

    TypePtr parseFunctionPointerDeclarator(
        const TypePtr &returnType,
        std::string &name)
    {
        take(TokenKind::LeftParen, "'('");
        CvmCallingConvention convention =
            target_.architecture == CVM_ARCH_X64
                ? CVM_CALL_WIN64
                : CVM_CALL_CDECL;
        if (at(TokenKind::Identifier) &&
            (current().text == "cdecl" ||
             current().text == "stdcall")) {
            convention =
                current().text == "stdcall"
                    ? CVM_CALL_STDCALL
                    : CVM_CALL_CDECL;
            ++position_;
        }
        take(TokenKind::Star, "'*'");
        name = take(
            TokenKind::Identifier,
            "function pointer name").text;
        take(TokenKind::RightParen, "')'");
        take(TokenKind::LeftParen, "'('");

        auto type = makeType(
            TypeKind::FunctionPointer,
            target_.pointerSize,
            target_.pointerAlignment);
        type->returnType = returnType;
        type->callingConvention = convention;
        if (!at(TokenKind::RightParen)) {
            if (at(TokenKind::KwVoid) &&
                position_ + 1 < tokens_.size() &&
                tokens_[position_ + 1].kind ==
                    TokenKind::RightParen) {
                ++position_;
            } else {
                for (;;) {
                    if (accept(TokenKind::Ellipsis)) {
                        if (convention == CVM_CALL_STDCALL)
                            fail(
                                "stdcall function pointer cannot be variadic");
                        type->variadic = true;
                        break;
                    }
                    TypePtr parameter =
                        parsePointerSuffix(parseType());
                    if (at(TokenKind::Identifier))
                        ++position_;
                    if (parameter->kind == TypeKind::Array)
                        parameter =
                            pointerType(parameter->element, target_);
                    type->parameterTypes.push_back(parameter);
                    if (!accept(TokenKind::Comma))
                        break;
                }
            }
        }
        take(TokenKind::RightParen, "')'");
        return type;
    }

    std::int64_t parseIntegerConstantExpression(int minimumPrecedence = 0)
    {
        std::int64_t left = 0;
        if (accept(TokenKind::LeftParen)) {
            left = parseIntegerConstantExpression();
            take(TokenKind::RightParen, "')'");
        } else if (accept(TokenKind::Minus)) {
            left = -parseIntegerConstantExpression(13);
        } else if (accept(TokenKind::Plus)) {
            left = parseIntegerConstantExpression(13);
        } else if (accept(TokenKind::BitNot)) {
            left = ~parseIntegerConstantExpression(13);
        } else if (at(TokenKind::Integer) ||
                   at(TokenKind::Character)) {
            left = current().integer;
            ++position_;
        } else if (at(TokenKind::Identifier)) {
            const auto constant = enumConstants_.find(current().text);
            if (constant == enumConstants_.end())
                fail("expected an integer constant expression");
            left = constant->second;
            ++position_;
        } else {
            fail("expected an integer constant expression");
        }

        for (;;) {
            const TokenKind operation = current().kind;
            const int operationPrecedence = precedence(operation);
            if (operationPrecedence < minimumPrecedence ||
                operation == TokenKind::Assign ||
                operation == TokenKind::AddAssign ||
                operation == TokenKind::SubtractAssign ||
                operation == TokenKind::Question ||
                operation == TokenKind::LogicalAnd ||
                operation == TokenKind::LogicalOr ||
                operation == TokenKind::Equal ||
                operation == TokenKind::NotEqual ||
                operation == TokenKind::Less ||
                operation == TokenKind::LessEqual ||
                operation == TokenKind::Greater ||
                operation == TokenKind::GreaterEqual) {
                break;
            }
            ++position_;
            const std::int64_t right =
                parseIntegerConstantExpression(operationPrecedence + 1);
            switch (operation) {
            case TokenKind::Plus: left += right; break;
            case TokenKind::Minus: left -= right; break;
            case TokenKind::Star: left *= right; break;
            case TokenKind::Slash:
                if (right == 0)
                    fail("division by zero in constant expression");
                left /= right;
                break;
            case TokenKind::Percent:
                if (right == 0)
                    fail("division by zero in constant expression");
                left %= right;
                break;
            case TokenKind::ShiftLeft: left <<= right; break;
            case TokenKind::ShiftRight: left >>= right; break;
            case TokenKind::BitAnd: left &= right; break;
            case TokenKind::BitOr: left |= right; break;
            case TokenKind::BitXor: left ^= right; break;
            default:
                fail("invalid operator in integer constant expression");
            }
        }
        return left;
    }

    TypePtr parseArraySuffix(TypePtr base)
    {
        std::vector<std::uint32_t> dimensions;
        while (accept(TokenKind::LeftBracket)) {
            std::uint32_t elementCount = 0;
            if (!at(TokenKind::RightBracket)) {
                const std::int64_t count =
                    parseIntegerConstantExpression();
                if (count <= 0 || count > UINT32_MAX)
                    fail("invalid array size");
                elementCount =
                    static_cast<std::uint32_t>(count);
            }
            take(TokenKind::RightBracket, "']'");
            dimensions.push_back(elementCount);
        }
        for (auto dimension = dimensions.rbegin();
             dimension != dimensions.rend();
             ++dimension) {
            const std::uint32_t elementCount = *dimension;
            if (elementCount != 0 &&
                elementCount > UINT32_MAX / base->size)
                fail("invalid array size");
            auto array = makeType(
                TypeKind::Array,
                elementCount * base->size,
                base->alignment);
            array->element = base;
            array->elementCount = elementCount;
            base = array;
        }
        return base;
    }

    void inferArrayBound(TypePtr &type)
    {
        if (type->kind != TypeKind::Array ||
            type->elementCount != 0 ||
            !at(TokenKind::Assign)) {
            return;
        }
        std::uint32_t count = 0;
        if (tokens_[position_ + 1].kind == TokenKind::String &&
            type->element->kind == TypeKind::Character) {
            count = static_cast<std::uint32_t>(
                tokens_[position_ + 1].text.size() + 1);
        } else if (tokens_[position_ + 1].kind == TokenKind::LeftBrace) {
            int depth = 0;
            bool hasValue = false;
            for (std::size_t at = position_ + 1;
                 at < tokens_.size();
                 ++at) {
                if (tokens_[at].kind == TokenKind::LeftBrace) {
                    ++depth;
                    if (depth == 1)
                        continue;
                } else if (tokens_[at].kind == TokenKind::RightBrace) {
                    if (depth == 1) {
                        if (hasValue)
                            ++count;
                        break;
                    }
                    --depth;
                } else if (tokens_[at].kind == TokenKind::Comma &&
                           depth == 1) {
                    ++count;
                    hasValue = false;
                    continue;
                }
                if (depth == 1)
                    hasValue = true;
            }
        }
        if (count == 0)
            fail("array bound cannot be inferred");
        type->elementCount = count;
        type->size = count * type->element->size;
    }

    std::uint32_t storageSize(const TypePtr &type) const
    {
        return type->size;
    }

    std::uint32_t alignStorage(
        std::uint32_t offset,
        std::uint32_t alignment) const
    {
        return (offset + alignment - 1u) & ~(alignment - 1u);
    }

    void emitInitializer(const TypePtr &type)
    {
        if (type->kind == TypeKind::Array) {
            if (at(TokenKind::String) &&
                type->element->kind == TypeKind::Character) {
                const Token literal =
                    take(TokenKind::String, "string initializer");
                const std::uint32_t offset =
                    static_cast<std::uint32_t>(data_.size());
                data_.insert(
                    data_.end(),
                    literal.text.begin(),
                    literal.text.end());
                data_.push_back(0);
                emit(makeInstruction(
                    CVM_OP_PUSH_CONSTANT_ADDRESS,
                    CVM_TYPE_CSTR,
                    static_cast<std::int32_t>(offset)));
                emit(makeInstruction(
                    CVM_OP_COPY_BYTES,
                    CVM_TYPE_VOID,
                    static_cast<std::int32_t>(
                        std::min<std::uint32_t>(
                            type->size,
                            static_cast<std::uint32_t>(
                                literal.text.size() + 1)))));
                emit(makeInstruction(CVM_OP_POP));
                return;
            }

            const bool braced = accept(TokenKind::LeftBrace);
            std::uint32_t index = 0;
            while (index < type->elementCount &&
                   !(braced && at(TokenKind::RightBrace)) &&
                   !at(TokenKind::RightBrace)) {
                if (index >= type->elementCount)
                    fail("too many array initializer elements");
                emit(makeInstruction(CVM_OP_DUP));
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I32,
                    static_cast<std::int32_t>(index)));
                emit(makeInstruction(
                    CVM_OP_POINTER_INDEX,
                    CVM_TYPE_PTR,
                    static_cast<std::int32_t>(
                        type->element->size)));
                emitInitializer(type->element);
                ++index;
                if (index == type->elementCount)
                    break;
                if (!accept(TokenKind::Comma))
                    break;
                if (at(TokenKind::RightBrace))
                    break;
            }
            if (braced) {
                (void)accept(TokenKind::Comma);
                if (!at(TokenKind::RightBrace))
                    fail("too many array initializer elements");
                take(TokenKind::RightBrace, "'}'");
            }
            emit(makeInstruction(CVM_OP_POP));
            return;
        }
        if (type->kind == TypeKind::Structure ||
            type->kind == TypeKind::Union) {
            take(TokenKind::LeftBrace, "'{'");
            const std::size_t fieldCount =
                type->kind == TypeKind::Union
                    ? std::min<std::size_t>(1, type->fields.size())
                    : type->fields.size();
            std::size_t fieldIndex = 0;
            while (fieldIndex < fieldCount &&
                   !at(TokenKind::RightBrace)) {
                const CField &field = type->fields[fieldIndex];
                emit(makeInstruction(CVM_OP_DUP));
                if (field.offset != 0) {
                    emit(makeInstruction(
                        CVM_OP_PUSH_IMMEDIATE,
                        CVM_TYPE_I32,
                        static_cast<std::int32_t>(field.offset)));
                    emit(makeInstruction(
                        CVM_OP_POINTER_ADD,
                        CVM_TYPE_PTR));
                }
                emitInitializer(field.type);
                ++fieldIndex;
                if (fieldIndex == fieldCount)
                    break;
                if (!accept(TokenKind::Comma))
                    break;
            }
            (void)accept(TokenKind::Comma);
            take(TokenKind::RightBrace, "'}'");
            emit(makeInstruction(CVM_OP_POP));
            return;
        }

        Expression expression = parseExpression(1);
        makeRvalue(expression);
        emitConversion(expression.type, type);
        emit(makeInstruction(CVM_OP_STORE, vmType(type)));
        emit(makeInstruction(CVM_OP_POP));
    }

    void emit(CvmInstruction instruction)
    {
        function_->code.push_back(instruction);
    }

    std::uint32_t emitJump(CvmOpcode opcode)
    {
        const std::uint32_t index =
            static_cast<std::uint32_t>(function_->code.size());
        emit(makeInstruction(opcode, CVM_TYPE_VOID, -1));
        return index;
    }

    void patchJump(std::uint32_t instruction, std::uint32_t target)
    {
        function_->code[instruction].a =
            static_cast<std::int32_t>(target);
    }

    std::uint32_t codePosition() const
    {
        return static_cast<std::uint32_t>(function_->code.size());
    }

    void resolveGotos(Function &function)
    {
        for (const auto &[instruction, label] : function.gotos) {
            const auto target = function.labels.find(label);
            if (target == function.labels.end())
                fail("unknown label '" + label + "'");
            function.code[instruction].a =
                static_cast<std::int32_t>(target->second);
        }
    }

    std::int64_t parseCaseConstant()
    {
        bool negative = false;
        if (accept(TokenKind::Minus))
            negative = true;
        else
            (void)accept(TokenKind::Plus);

        std::int64_t value = 0;
        if (at(TokenKind::Integer) || at(TokenKind::Character)) {
            value = current().integer;
            ++position_;
        } else if (at(TokenKind::Identifier)) {
            const std::string name = current().text;
            const auto constant = enumConstants_.find(name);
            if (constant == enumConstants_.end())
                fail("case value must be an integer constant");
            value = constant->second;
            ++position_;
        } else {
            fail("case value must be an integer constant");
        }
        return negative ? -value : value;
    }

    void parseFunction()
    {
        const auto savedScopes = std::move(localScopes_);
        localScopes_.clear();
        localScopes_.push_back({});
        Function parsed;
        if (isTypeStart(current().kind)) {
            parsed.returnType = parseType();
        } else {
            const auto prototype = prototypes_.find(current().text);
            parsed.returnType =
                prototype != prototypes_.end()
                    ? prototype->second.returnType
                    : intType();
        }
        parsed.name = take(TokenKind::Identifier, "function name").text;
        take(TokenKind::LeftParen, "'('");

        std::uint32_t parameterOffset = 0;
        std::vector<TypePtr> declaredParameterTypes;
        if (!at(TokenKind::RightParen)) {
            if (at(TokenKind::KwVoid)) {
                take(TokenKind::KwVoid, "'void'");
            } else {
                for (;;) {
                    TypePtr type = parseType();
                    type = parsePointerSuffix(type);
                    if (type->kind == TypeKind::Void)
                        fail("parameter cannot have void type");
                    std::string name;
                    if (at(TokenKind::Identifier))
                        name = take(
                            TokenKind::Identifier,
                            "parameter name").text;
                    else
                        name = "__parameter_" +
                            std::to_string(parsed.parameters.size());
                    if (at(TokenKind::LeftBracket))
                        type = parseArraySuffix(type);
                    if (type->kind == TypeKind::Array)
                        type = pointerType(type->element, target_);
                    declaredParameterTypes.push_back(type);
                    parameterOffset =
                        alignStorage(parameterOffset, type->alignment);
                    parsed.localAlignment =
                        std::max(parsed.localAlignment, type->alignment);
                    CvmParameter parameter{};
                    parameter.frame_offset = parameterOffset;
                    parameter.value_type =
                        static_cast<std::uint8_t>(vmType(type));
                    parsed.parameters.push_back({name, parameter});
                    parsed.variables.emplace(
                        name,
                        Variable{parameterOffset, false, type});
                    localScopes_.back().emplace(
                        name,
                        Variable{parameterOffset, false, type});
                    parameterOffset += storageSize(type);
                    if (!accept(TokenKind::Comma))
                        break;
                }
            }
        }
        take(TokenKind::RightParen, "')'");

        if (accept(TokenKind::Semicolon)) {
            prototypes_[parsed.name] = FunctionPrototype{
                parsed.returnType,
                std::move(declaredParameterTypes)};
            localScopes_ = savedScopes;
            return;
        }

        parsed.localBytes = parameterOffset;
        functions_.push_back(std::move(parsed));
        function_ = &functions_.back();

        take(TokenKind::LeftBrace, "'{'");
        while (!at(TokenKind::RightBrace))
            parseStatement();
        take(TokenKind::RightBrace, "'}'");

        resolveGotos(*function_);

        /*
         * Keep a concrete terminal instruction even when all source paths
         * appear to return. Branches emitted for structured statements may
         * legally target the lexical end of the body; the terminal prevents
         * that target from falling into the next function after code linking.
         * A later control-flow analysis diagnoses reachable missing returns.
         */
        if (function_->returnType->kind == TypeKind::Void) {
            emit(makeInstruction(CVM_OP_RETURN, CVM_TYPE_VOID));
        } else {
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                vmType(function_->returnType),
                0));
            emit(makeInstruction(
                CVM_OP_RETURN,
                vmType(function_->returnType)));
        }
        function_ = nullptr;
        localScopes_ = savedScopes;
    }

    void emitAddress(const Variable &variable)
    {
        emit(makeInstruction(
            variable.global
                ? CVM_OP_ADDRESS_GLOBAL
                : CVM_OP_ADDRESS_LOCAL,
            CVM_TYPE_PTR,
            static_cast<std::int32_t>(variable.offset)));
    }

    const Variable *lookupVariable(const std::string &name) const
    {
        for (auto scope = localScopes_.rbegin();
             scope != localScopes_.rend();
             ++scope) {
            const auto local = scope->find(name);
            if (local != scope->end())
                return &local->second;
        }
        const auto global = globals_.find(name);
        if (global != globals_.end())
            return &global->second;
        return nullptr;
    }

    const Variable &findVariable(const std::string &name) const
    {
        const Variable *variable = lookupVariable(name);
        if (variable != nullptr)
            return *variable;
        fail("unknown variable '" + name + "'");
    }

    void parseGlobalDeclaration()
    {
        const TypePtr baseType = parseType();
        if (accept(TokenKind::Semicolon))
            return;
        if (at(TokenKind::LeftParen)) {
            std::string name;
            TypePtr type =
                parseFunctionPointerDeclarator(baseType, name);
            if (globals_.count(name) != 0)
                fail("duplicate global variable '" + name + "'");
            globalBytes_ =
                alignStorage(globalBytes_, type->alignment);
            const Variable variable{globalBytes_, true, type};
            globalBytes_ += storageSize(type);
            globals_.emplace(name, variable);
            if (accept(TokenKind::Assign)) {
                emitAddress(variable);
                emitInitializer(type);
            }
            take(TokenKind::Semicolon, "';'");
            return;
        }
        for (;;) {
            TypePtr type = parsePointerSuffix(baseType);
            const std::string name =
                take(TokenKind::Identifier, "global variable name").text;
            type = parseArraySuffix(type);
            inferArrayBound(type);
            if (globals_.count(name) != 0)
                fail("duplicate global variable '" + name + "'");
            globalBytes_ = alignStorage(globalBytes_, type->alignment);
            const Variable variable{globalBytes_, true, type};
            globalBytes_ += storageSize(type);
            globals_.emplace(name, variable);
            if (accept(TokenKind::Assign)) {
                emitAddress(variable);
                emitInitializer(type);
            }
            if (!accept(TokenKind::Comma))
                break;
        }
        take(TokenKind::Semicolon, "';'");
    }

    void parseStatement()
    {
        if (at(TokenKind::Identifier) &&
            position_ + 1 < tokens_.size() &&
            tokens_[position_ + 1].kind == TokenKind::Colon) {
            const std::string label =
                take(TokenKind::Identifier, "label").text;
            take(TokenKind::Colon, "':'");
            if (!function_->labels.emplace(label, codePosition()).second)
                fail("duplicate label '" + label + "'");
            return;
        }
        if (accept(TokenKind::KwGoto)) {
            const std::string label =
                take(TokenKind::Identifier, "goto label").text;
            const std::uint32_t jump = emitJump(CVM_OP_JUMP);
            function_->gotos.push_back({jump, label});
            take(TokenKind::Semicolon, "';'");
            return;
        }
        if (accept(TokenKind::KwCase)) {
            if (switches_.empty())
                fail("case is not inside a switch");
            const std::int64_t value = parseCaseConstant();
            take(TokenKind::Colon, "':'");
            for (const auto &existing : switches_.back().cases) {
                if (existing.first == value)
                    fail("duplicate case value");
            }
            switches_.back().cases.push_back({value, codePosition()});
            return;
        }
        if (accept(TokenKind::KwDefault)) {
            if (switches_.empty())
                fail("default is not inside a switch");
            take(TokenKind::Colon, "':'");
            if (switches_.back().defaultTarget != CVM_NO_INDEX)
                fail("duplicate default label");
            switches_.back().defaultTarget = codePosition();
            return;
        }
        if (accept(TokenKind::Semicolon))
            return;
        if (accept(TokenKind::LeftBrace)) {
            localScopes_.push_back({});
            while (!at(TokenKind::RightBrace))
                parseStatement();
            take(TokenKind::RightBrace, "'}'");
            localScopes_.pop_back();
            return;
        }
        if (accept(TokenKind::KwIf)) {
            take(TokenKind::LeftParen, "'('");
            Expression condition = parseExpression();
            makeRvalue(condition);
            take(TokenKind::RightParen, "')'");
            const std::uint32_t falseJump =
                emitJump(CVM_OP_JUMP_IF_ZERO);
            parseStatement();
            if (accept(TokenKind::KwElse)) {
                const std::uint32_t endJump = emitJump(CVM_OP_JUMP);
                patchJump(falseJump, codePosition());
                parseStatement();
                patchJump(endJump, codePosition());
            } else {
                patchJump(falseJump, codePosition());
            }
            return;
        }
        if (accept(TokenKind::KwSwitch)) {
            take(TokenKind::LeftParen, "'('");
            function_->localBytes =
                alignStorage(function_->localBytes, alignof(std::uint64_t));
            function_->localAlignment = std::max<std::uint32_t>(
                function_->localAlignment,
                alignof(std::uint64_t));
            const std::uint32_t valueOffset = function_->localBytes;
            function_->localBytes += sizeof(std::uint64_t);
            emit(makeInstruction(
                CVM_OP_ADDRESS_LOCAL,
                CVM_TYPE_PTR,
                static_cast<std::int32_t>(valueOffset)));
            Expression value = parseExpression();
            makeRvalue(value);
            take(TokenKind::RightParen, "')'");
            emit(makeInstruction(CVM_OP_STORE, vmType(value.type)));
            emit(makeInstruction(CVM_OP_POP));
            const std::uint32_t dispatchJump = emitJump(CVM_OP_JUMP);

            SwitchContext context;
            context.type = value.type;
            context.valueOffset = valueOffset;
            context.nesting = ++statementNesting_;
            switches_.push_back(std::move(context));
            parseStatement();
            const std::uint32_t bodyExit = emitJump(CVM_OP_JUMP);
            const std::uint32_t dispatch = codePosition();
            patchJump(dispatchJump, dispatch);

            SwitchContext completed = std::move(switches_.back());
            switches_.pop_back();
            for (const auto &[caseValue, target] : completed.cases) {
                emit(makeInstruction(
                    CVM_OP_ADDRESS_LOCAL,
                    CVM_TYPE_PTR,
                    static_cast<std::int32_t>(completed.valueOffset)));
                emit(makeInstruction(CVM_OP_LOAD, vmType(completed.type)));
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    vmType(completed.type),
                    static_cast<std::int32_t>(caseValue),
                    static_cast<std::int32_t>(
                        static_cast<std::uint64_t>(caseValue) >> 32)));
                emit(makeInstruction(
                    CVM_OP_COMPARE_EQUAL,
                    vmType(completed.type)));
                emit(makeInstruction(
                    CVM_OP_JUMP_IF_NONZERO,
                    CVM_TYPE_VOID,
                    static_cast<std::int32_t>(target)));
            }
            const std::uint32_t finalJump = emitJump(CVM_OP_JUMP);
            const std::uint32_t end = codePosition();
            patchJump(
                finalJump,
                completed.defaultTarget == CVM_NO_INDEX
                    ? end
                    : completed.defaultTarget);
            patchJump(bodyExit, end);
            for (std::uint32_t jump : completed.breaks)
                patchJump(jump, end);
            return;
        }
        if (accept(TokenKind::KwWhile)) {
            const std::uint32_t conditionPosition = codePosition();
            take(TokenKind::LeftParen, "'('");
            Expression condition = parseExpression();
            makeRvalue(condition);
            take(TokenKind::RightParen, "')'");
            const std::uint32_t exitJump =
                emitJump(CVM_OP_JUMP_IF_ZERO);
            loops_.push_back({});
            loops_.back().nesting = ++statementNesting_;
            parseStatement();
            emit(makeInstruction(
                CVM_OP_JUMP,
                CVM_TYPE_VOID,
                static_cast<std::int32_t>(conditionPosition)));
            const std::uint32_t end = codePosition();
            patchJump(exitJump, end);
            for (std::uint32_t jump : loops_.back().breaks)
                patchJump(jump, end);
            for (std::uint32_t jump : loops_.back().continues)
                patchJump(jump, conditionPosition);
            loops_.pop_back();
            return;
        }
        if (accept(TokenKind::KwDo)) {
            const std::uint32_t bodyPosition = codePosition();
            loops_.push_back({});
            loops_.back().nesting = ++statementNesting_;
            parseStatement();
            take(TokenKind::KwWhile, "'while'");
            const std::uint32_t conditionPosition = codePosition();
            take(TokenKind::LeftParen, "'('");
            Expression condition = parseExpression();
            makeRvalue(condition);
            take(TokenKind::RightParen, "')'");
            take(TokenKind::Semicolon, "';'");
            emit(makeInstruction(
                CVM_OP_JUMP_IF_NONZERO,
                CVM_TYPE_VOID,
                static_cast<std::int32_t>(bodyPosition)));
            const std::uint32_t end = codePosition();
            for (std::uint32_t jump : loops_.back().breaks)
                patchJump(jump, end);
            for (std::uint32_t jump : loops_.back().continues)
                patchJump(jump, conditionPosition);
            loops_.pop_back();
            return;
        }
        if (accept(TokenKind::KwFor)) {
            localScopes_.push_back({});
            take(TokenKind::LeftParen, "'('");
            if (!accept(TokenKind::Semicolon)) {
                if (isTypeStart(current().kind))
                    parseStatement();
                else {
                    Expression initializer = parseExpression();
                    makeRvalue(initializer);
                    if (initializer.type->kind != TypeKind::Void)
                        emit(makeInstruction(CVM_OP_POP));
                    take(TokenKind::Semicolon, "';'");
                }
            }
            const std::uint32_t conditionPosition = codePosition();
            std::uint32_t exitJump = CVM_NO_INDEX;
            if (!accept(TokenKind::Semicolon)) {
                Expression condition = parseExpression();
                makeRvalue(condition);
                take(TokenKind::Semicolon, "';'");
                exitJump = emitJump(CVM_OP_JUMP_IF_ZERO);
            }

            const std::uint32_t incrementStart = codePosition();
            if (!at(TokenKind::RightParen)) {
                Expression increment = parseExpression();
                makeRvalue(increment);
                if (increment.type->kind != TypeKind::Void)
                    emit(makeInstruction(CVM_OP_POP));
            }
            take(TokenKind::RightParen, "')'");
            std::vector<CvmInstruction> incrementCode(
                function_->code.begin() + incrementStart,
                function_->code.end());
            function_->code.erase(
                function_->code.begin() + incrementStart,
                function_->code.end());

            loops_.push_back({});
            loops_.back().nesting = ++statementNesting_;
            parseStatement();
            const std::uint32_t continuePosition = codePosition();
            function_->code.insert(
                function_->code.end(),
                incrementCode.begin(),
                incrementCode.end());
            emit(makeInstruction(
                CVM_OP_JUMP,
                CVM_TYPE_VOID,
                static_cast<std::int32_t>(conditionPosition)));
            const std::uint32_t end = codePosition();
            if (exitJump != CVM_NO_INDEX)
                patchJump(exitJump, end);
            for (std::uint32_t jump : loops_.back().breaks)
                patchJump(jump, end);
            for (std::uint32_t jump : loops_.back().continues)
                patchJump(jump, continuePosition);
            loops_.pop_back();
            localScopes_.pop_back();
            return;
        }
        if (accept(TokenKind::KwBreak)) {
            const bool useSwitch =
                !switches_.empty() &&
                (loops_.empty() ||
                 switches_.back().nesting > loops_.back().nesting);
            if (useSwitch)
                switches_.back().breaks.push_back(emitJump(CVM_OP_JUMP));
            else if (!loops_.empty())
                loops_.back().breaks.push_back(emitJump(CVM_OP_JUMP));
            else
                fail("break is not inside a loop or switch");
            take(TokenKind::Semicolon, "';'");
            return;
        }
        if (accept(TokenKind::KwContinue)) {
            if (loops_.empty())
                fail("continue is not inside a loop");
            loops_.back().continues.push_back(emitJump(CVM_OP_JUMP));
            take(TokenKind::Semicolon, "';'");
            return;
        }
        if (accept(TokenKind::KwReturn)) {
            if (function_->returnType->kind == TypeKind::Void) {
                take(TokenKind::Semicolon, "';'");
                emit(makeInstruction(CVM_OP_RETURN, CVM_TYPE_VOID));
            } else {
                Expression expression = parseExpression();
                makeRvalue(expression);
                emitConversion(expression.type, function_->returnType);
                take(TokenKind::Semicolon, "';'");
                emit(makeInstruction(
                    CVM_OP_RETURN,
                    vmType(function_->returnType)));
            }
            function_->hasReturn = true;
            return;
        }

        if (isTypeStart(current().kind)) {
            const bool isStatic = at(TokenKind::KwStatic);
            const TypePtr baseType = parseType();
            if (at(TokenKind::LeftParen)) {
                std::string name;
                TypePtr type =
                    parseFunctionPointerDeclarator(baseType, name);
                if (localScopes_.back().count(name) != 0)
                    fail("duplicate local variable '" + name + "'");
                function_->localBytes =
                    alignStorage(
                        function_->localBytes,
                        type->alignment);
                function_->localAlignment = std::max(
                    function_->localAlignment,
                    type->alignment);
                const std::uint32_t offset =
                    function_->localBytes;
                function_->localBytes += storageSize(type);
                const Variable variable{offset, false, type};
                localScopes_.back().emplace(name, variable);
                if (accept(TokenKind::Assign)) {
                    emit(makeInstruction(
                        CVM_OP_ADDRESS_LOCAL,
                        CVM_TYPE_PTR,
                        static_cast<std::int32_t>(offset)));
                    emitInitializer(type);
                }
                take(TokenKind::Semicolon, "';'");
                return;
            }
            for (;;) {
                TypePtr type = parsePointerSuffix(baseType);
                const std::string name =
                    take(TokenKind::Identifier, "local variable name").text;
                type = parseArraySuffix(type);
                inferArrayBound(type);
                if (localScopes_.back().count(name) != 0)
                    fail("duplicate local variable '" + name + "'");
                std::uint32_t offset = 0;
                if (isStatic) {
                    globalBytes_ =
                        alignStorage(globalBytes_, type->alignment);
                    offset = globalBytes_;
                    globalBytes_ += storageSize(type);
                } else {
                    function_->localBytes =
                        alignStorage(
                            function_->localBytes,
                            type->alignment);
                    function_->localAlignment = std::max(
                        function_->localAlignment,
                        type->alignment);
                    offset = function_->localBytes;
                    function_->localBytes += storageSize(type);
                }
                const Variable variable{offset, isStatic, type};
                localScopes_.back().emplace(name, variable);
                if (accept(TokenKind::Assign)) {
                    if (isStatic) {
                        Function *savedFunction = function_;
                        function_ = &functions_[0];
                        emitAddress(variable);
                        emitInitializer(type);
                        function_ = savedFunction;
                    } else {
                        emit(makeInstruction(
                            CVM_OP_ADDRESS_LOCAL,
                            CVM_TYPE_PTR,
                            static_cast<std::int32_t>(offset)));
                        emitInitializer(type);
                    }
                }
                if (!accept(TokenKind::Comma))
                    break;
            }
            take(TokenKind::Semicolon, "';'");
            return;
        }

        Expression expression = parseExpression();
        take(TokenKind::Semicolon, "';'");
        makeRvalue(expression);
        if (expression.type->kind != TypeKind::Void)
            emit(makeInstruction(CVM_OP_POP));
    }

    int precedence(TokenKind kind) const
    {
        if (kind == TokenKind::Comma)
            return 0;
        if (kind == TokenKind::Assign ||
            kind == TokenKind::AddAssign ||
            kind == TokenKind::SubtractAssign ||
            kind == TokenKind::MultiplyAssign ||
            kind == TokenKind::DivideAssign ||
            kind == TokenKind::ModuloAssign)
            return 1;
        if (kind == TokenKind::Question)
            return 2;
        if (kind == TokenKind::LogicalOr)
            return 3;
        if (kind == TokenKind::LogicalAnd)
            return 4;
        if (kind == TokenKind::BitOr)
            return 5;
        if (kind == TokenKind::BitXor)
            return 6;
        if (kind == TokenKind::BitAnd)
            return 7;
        if (kind == TokenKind::Equal || kind == TokenKind::NotEqual)
            return 8;
        if (kind == TokenKind::Less || kind == TokenKind::LessEqual ||
            kind == TokenKind::Greater || kind == TokenKind::GreaterEqual)
            return 9;
        if (kind == TokenKind::ShiftLeft || kind == TokenKind::ShiftRight)
            return 10;
        if (kind == TokenKind::Plus || kind == TokenKind::Minus)
            return 11;
        if (kind == TokenKind::Star || kind == TokenKind::Slash ||
            kind == TokenKind::Percent)
            return 12;
        return -1;
    }

    void makeRvalue(Expression &expression)
    {
        if (!expression.lvalue)
            return;
        if (expression.type->kind == TypeKind::Array) {
            expression.type =
                pointerType(expression.type->element, target_);
            expression.lvalue = false;
            return;
        }
        emit(makeInstruction(CVM_OP_LOAD, vmType(expression.type)));
        expression.lvalue = false;
    }

    bool isFloatingType(const TypePtr &type) const
    {
        return type->kind == TypeKind::Float ||
               type->kind == TypeKind::Double;
    }

    TypePtr commonArithmeticType(
        const TypePtr &left,
        const TypePtr &right) const
    {
        if (left->kind == TypeKind::Double ||
            right->kind == TypeKind::Double)
            return scalarType(TypeKind::Double, false);
        if (left->kind == TypeKind::Float ||
            right->kind == TypeKind::Float)
            return scalarType(TypeKind::Float, false);
        if (left->size >= 8 || right->size >= 8)
            return scalarType(
                TypeKind::LongLong,
                left->isUnsigned || right->isUnsigned);
        return scalarType(
            TypeKind::Integer,
            left->isUnsigned || right->isUnsigned);
    }

    void emitConversion(const TypePtr &source, const TypePtr &target)
    {
        if (vmType(source) == vmType(target))
            return;
        emit(makeInstruction(
            CVM_OP_CONVERT,
            vmType(target),
            static_cast<std::int32_t>(vmType(source))));
    }

    Expression parseExpression(int minimumPrecedence = 0)
    {
        Expression left = parsePrefix();
        while (precedence(current().kind) >= minimumPrecedence) {
            const TokenKind operation = tokens_[position_++].kind;
            const int operationPrecedence = precedence(operation);
            if (operation == TokenKind::Comma) {
                makeRvalue(left);
                if (left.type->kind != TypeKind::Void)
                    emit(makeInstruction(CVM_OP_POP));
                left = parseExpression(1);
                continue;
            }
            if (operation == TokenKind::Question) {
                makeRvalue(left);
                const std::uint32_t falseJump =
                    emitJump(CVM_OP_JUMP_IF_ZERO);
                Expression whenTrue = parseExpression();
                makeRvalue(whenTrue);
                take(TokenKind::Colon, "':'");
                const std::uint32_t trueConversion = codePosition();
                emit(makeInstruction(
                    CVM_OP_CONVERT,
                    vmType(whenTrue.type),
                    static_cast<std::int32_t>(
                        vmType(whenTrue.type))));
                const std::uint32_t endJump = emitJump(CVM_OP_JUMP);
                patchJump(falseJump, codePosition());
                Expression whenFalse =
                    parseExpression(operationPrecedence);
                makeRvalue(whenFalse);
                const TypePtr resultType =
                    commonArithmeticType(
                        whenTrue.type,
                        whenFalse.type);
                function_->code[trueConversion].type =
                    static_cast<std::uint8_t>(vmType(resultType));
                function_->code[trueConversion].a =
                    static_cast<std::int32_t>(
                        vmType(whenTrue.type));
                emitConversion(whenFalse.type, resultType);
                patchJump(endJump, codePosition());
                left = {resultType, false};
                continue;
            }
            if (operation == TokenKind::Assign ||
                operation == TokenKind::AddAssign ||
                operation == TokenKind::SubtractAssign ||
                operation == TokenKind::MultiplyAssign ||
                operation == TokenKind::DivideAssign ||
                operation == TokenKind::ModuloAssign) {
                if (!left.lvalue)
                    fail("assignment requires a modifiable lvalue");
                if (left.type->kind == TypeKind::Array ||
                    left.type->kind == TypeKind::Structure ||
                    left.type->kind == TypeKind::Union) {
                    if (operation != TokenKind::Assign)
                        fail("aggregate compound assignment is invalid");
                    Expression right =
                        parseExpression(operationPrecedence);
                    if (!right.lvalue || right.type->kind != left.type->kind ||
                        right.type->size != left.type->size)
                        fail("incompatible aggregate assignment");
                    emit(makeInstruction(
                        CVM_OP_COPY_BYTES,
                        CVM_TYPE_VOID,
                        static_cast<std::int32_t>(left.type->size)));
                    left.type = pointerType(left.type, target_);
                    left.lvalue = false;
                    continue;
                }
                if (operation != TokenKind::Assign) {
                    emit(makeInstruction(CVM_OP_DUP));
                    emit(makeInstruction(CVM_OP_LOAD, vmType(left.type)));
                }
                Expression right = parseExpression(operationPrecedence);
                makeRvalue(right);
                emitConversion(right.type, left.type);
                if (left.type->kind == TypeKind::Pointer &&
                    (operation == TokenKind::AddAssign ||
                     operation == TokenKind::SubtractAssign)) {
                    emit(makeInstruction(
                        CVM_OP_POINTER_INDEX,
                        CVM_TYPE_PTR,
                        operation == TokenKind::AddAssign
                            ? static_cast<std::int32_t>(
                                  left.type->element->size)
                            : -static_cast<std::int32_t>(
                                  left.type->element->size)));
                } else if (operation == TokenKind::AddAssign) {
                    emit(makeInstruction(CVM_OP_ADD, vmType(left.type)));
                } else if (operation == TokenKind::SubtractAssign) {
                    emit(makeInstruction(
                        CVM_OP_SUBTRACT,
                        vmType(left.type)));
                }
                else if (operation == TokenKind::MultiplyAssign)
                    emit(makeInstruction(
                        CVM_OP_MULTIPLY,
                        vmType(left.type)));
                else if (operation == TokenKind::DivideAssign)
                    emit(makeInstruction(
                        left.type->isUnsigned
                            ? CVM_OP_DIVIDE_UNSIGNED
                            : CVM_OP_DIVIDE_SIGNED,
                        vmType(left.type)));
                else if (operation == TokenKind::ModuloAssign)
                    emit(makeInstruction(
                        left.type->isUnsigned
                            ? CVM_OP_MODULO_UNSIGNED
                            : CVM_OP_MODULO_SIGNED,
                        vmType(left.type)));
                emit(makeInstruction(CVM_OP_STORE, vmType(left.type)));
                left.lvalue = false;
                continue;
            }
            makeRvalue(left);
            if (operation == TokenKind::LogicalAnd ||
                operation == TokenKind::LogicalOr) {
                const std::uint32_t shortCircuit = emitJump(
                    operation == TokenKind::LogicalAnd
                        ? CVM_OP_JUMP_IF_ZERO
                        : CVM_OP_JUMP_IF_NONZERO);
                Expression right =
                    parseExpression(operationPrecedence + 1);
                makeRvalue(right);
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I32,
                    0));
                emit(makeInstruction(
                    CVM_OP_COMPARE_NOT_EQUAL,
                    CVM_TYPE_I32));
                const std::uint32_t endJump = emitJump(CVM_OP_JUMP);
                patchJump(shortCircuit, codePosition());
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I32,
                    operation == TokenKind::LogicalOr ? 1 : 0));
                patchJump(endJump, codePosition());
                left = {intType(), false};
                continue;
            }
            const std::size_t rightCodeStart = function_->code.size();
            Expression right = parseExpression(operationPrecedence + 1);
            makeRvalue(right);
            const bool leftPointer =
                left.type->kind == TypeKind::Pointer;
            const bool rightPointer =
                right.type->kind == TypeKind::Pointer;
            if ((operation == TokenKind::Plus ||
                 operation == TokenKind::Minus) &&
                leftPointer && !rightPointer) {
                emit(makeInstruction(
                    CVM_OP_POINTER_INDEX,
                    CVM_TYPE_PTR,
                    operation == TokenKind::Plus
                        ? static_cast<std::int32_t>(
                              left.type->element->size)
                        : -static_cast<std::int32_t>(
                              left.type->element->size)));
                left.lvalue = false;
                continue;
            }
            if (operation == TokenKind::Plus &&
                !leftPointer && rightPointer) {
                emit(makeInstruction(CVM_OP_SWAP));
                emit(makeInstruction(
                    CVM_OP_POINTER_INDEX,
                    CVM_TYPE_PTR,
                    static_cast<std::int32_t>(
                        right.type->element->size)));
                left = {right.type, false};
                continue;
            }
            if (operation == TokenKind::Minus &&
                leftPointer && rightPointer) {
                if (left.type->element->size !=
                    right.type->element->size)
                    fail("subtracting incompatible pointers");
                emit(makeInstruction(CVM_OP_SUBTRACT, CVM_TYPE_I64));
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I64,
                    static_cast<std::int32_t>(
                        left.type->element->size)));
                emit(makeInstruction(
                    CVM_OP_DIVIDE_SIGNED,
                    CVM_TYPE_I64));
                left = {
                    scalarType(TypeKind::LongLong, false),
                    false};
                continue;
            }
            TypePtr operationType =
                commonArithmeticType(left.type, right.type);
            {
                std::vector<CvmInstruction> rightCode(
                    function_->code.begin() + rightCodeStart,
                    function_->code.end());
                function_->code.erase(
                    function_->code.begin() + rightCodeStart,
                    function_->code.end());
                emitConversion(left.type, operationType);
                function_->code.insert(
                    function_->code.end(),
                    rightCode.begin(),
                    rightCode.end());
                emitConversion(right.type, operationType);
            }
            const CvmValueType binaryVmType = vmType(operationType);
            const bool unsignedOperation =
                operationType->isUnsigned;
            switch (operation) {
            case TokenKind::Plus:
                emit(makeInstruction(CVM_OP_ADD, binaryVmType));
                break;
            case TokenKind::Minus:
                emit(makeInstruction(CVM_OP_SUBTRACT, binaryVmType));
                break;
            case TokenKind::Star:
                emit(makeInstruction(CVM_OP_MULTIPLY, binaryVmType));
                break;
            case TokenKind::Slash:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_DIVIDE_UNSIGNED
                        : CVM_OP_DIVIDE_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::Percent:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_MODULO_UNSIGNED
                        : CVM_OP_MODULO_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::Equal:
                emit(makeInstruction(CVM_OP_COMPARE_EQUAL, binaryVmType));
                break;
            case TokenKind::NotEqual:
                emit(makeInstruction(
                    CVM_OP_COMPARE_NOT_EQUAL,
                    binaryVmType));
                break;
            case TokenKind::Less:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_COMPARE_LESS_UNSIGNED
                        : CVM_OP_COMPARE_LESS_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::LessEqual:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_COMPARE_LESS_EQUAL_UNSIGNED
                        : CVM_OP_COMPARE_LESS_EQUAL_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::Greater:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_COMPARE_GREATER_UNSIGNED
                        : CVM_OP_COMPARE_GREATER_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::GreaterEqual:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_COMPARE_GREATER_EQUAL_UNSIGNED
                        : CVM_OP_COMPARE_GREATER_EQUAL_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::BitAnd:
                emit(makeInstruction(CVM_OP_BIT_AND, binaryVmType));
                break;
            case TokenKind::BitOr:
                emit(makeInstruction(CVM_OP_BIT_OR, binaryVmType));
                break;
            case TokenKind::BitXor:
                emit(makeInstruction(CVM_OP_BIT_XOR, binaryVmType));
                break;
            case TokenKind::ShiftLeft:
                emit(makeInstruction(CVM_OP_SHIFT_LEFT, binaryVmType));
                break;
            case TokenKind::ShiftRight:
                emit(makeInstruction(
                    unsignedOperation
                        ? CVM_OP_SHIFT_RIGHT_UNSIGNED
                        : CVM_OP_SHIFT_RIGHT_SIGNED,
                    binaryVmType));
                break;
            case TokenKind::LogicalAnd:
                emit(makeInstruction(CVM_OP_LOGICAL_AND, CVM_TYPE_I32));
                break;
            case TokenKind::LogicalOr:
                emit(makeInstruction(CVM_OP_LOGICAL_OR, CVM_TYPE_I32));
                break;
            default:
                fail("invalid binary operator");
            }
            if (operation == TokenKind::Equal ||
                operation == TokenKind::NotEqual ||
                operation == TokenKind::Less ||
                operation == TokenKind::LessEqual ||
                operation == TokenKind::Greater ||
                operation == TokenKind::GreaterEqual) {
                left.type = intType();
            } else {
                left.type = operationType;
            }
            left.lvalue = false;
        }
        return left;
    }

    TypePtr findFunctionReturnType(const std::string &name) const
    {
        for (const Function &candidate : functions_) {
            if (candidate.name == name)
                return candidate.returnType;
        }
        const auto prototype = prototypes_.find(name);
        if (prototype != prototypes_.end())
            return prototype->second.returnType;
        if (name == "printf" || name == "sprintf" ||
            name == "strcmp" || name == "strncmp" ||
            name == "memcmp" ||
            name == "fclose" || name == "fgetc" ||
            name == "fprintf")
            return intType();
        if (name == "__picoc_argc")
            return intType();
        if (name == "__picoc_argv")
            return pointerType(
                pointerType(charType(), target_),
                target_);
        if (name == "strlen")
            return sizeType(target_);
        if (name == "fread" || name == "fwrite")
            return sizeType(target_);
        if (name == "strcpy" || name == "strncpy" ||
            name == "strcat" || name == "index" ||
            name == "rindex" || name == "fgets")
            return pointerType(charType(), target_);
        if (name == "memcpy" || name == "memset" ||
            name == "fopen")
            return pointerType(voidType(), target_);
        if (name == "sin" || name == "cos" || name == "tan" ||
            name == "asin" || name == "acos" || name == "atan" ||
            name == "sinh" || name == "cosh" || name == "tanh" ||
            name == "exp" || name == "fabs" || name == "log" ||
            name == "log10" || name == "pow" || name == "sqrt" ||
            name == "round" || name == "ceil" || name == "floor")
            return scalarType(TypeKind::Double, false);
        return intType();
    }

    TypePtr findFunctionParameterType(
        const std::string &name,
        std::size_t index) const
    {
        for (const Function &candidate : functions_) {
            if (candidate.name != name ||
                index >= candidate.parameters.size())
                continue;
            const auto variable =
                candidate.variables.find(
                    candidate.parameters[index].first);
            if (variable != candidate.variables.end())
                return variable->second.type;
        }
        const auto prototype = prototypes_.find(name);
        if (prototype != prototypes_.end() &&
            index < prototype->second.parameterTypes.size())
            return prototype->second.parameterTypes[index];
        return nullptr;
    }

    Expression parsePrefix()
    {
        if (accept(TokenKind::KwSizeof)) {
            TypePtr measured;
            if (accept(TokenKind::LeftParen)) {
                if (isTypeStart(current().kind)) {
                    measured = parsePointerSuffix(parseType());
                } else {
                    const std::size_t codeStart =
                        function_->code.size();
                    Expression expression = parseExpression();
                    measured = expression.type;
                    function_->code.resize(codeStart);
                }
                take(TokenKind::RightParen, "')'");
            } else {
                const std::size_t codeStart = function_->code.size();
                Expression expression = parsePrefix();
                measured = expression.type;
                function_->code.resize(codeStart);
            }
            const TypePtr resultType = sizeType(target_);
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                vmType(resultType),
                static_cast<std::int32_t>(measured->size),
                0));
            return {resultType, false};
        }
        if (at(TokenKind::LeftParen) &&
            position_ + 1 < tokens_.size() &&
            (tokens_[position_ + 1].kind == TokenKind::Identifier
                ? typedefs_.count(tokens_[position_ + 1].text) != 0
                : isTypeStart(tokens_[position_ + 1].kind))) {
            take(TokenKind::LeftParen, "'('");
            TypePtr target = parsePointerSuffix(parseType());
            take(TokenKind::RightParen, "')'");
            Expression expression = parsePrefix();
            makeRvalue(expression);
            emit(makeInstruction(
                CVM_OP_CONVERT,
                vmType(target),
                static_cast<std::int32_t>(vmType(expression.type))));
            return {target, false};
        }
        if (accept(TokenKind::BitAnd)) {
            Expression expression = parsePrefix();
            if (!expression.lvalue)
                fail("address-of requires an lvalue");
            expression.type = pointerType(expression.type, target_);
            expression.lvalue = false;
            return expression;
        }
        if (accept(TokenKind::Star)) {
            Expression expression = parsePrefix();
            makeRvalue(expression);
            if (!isPointerLike(expression.type))
                fail("dereference requires a pointer");
            return {
                expression.type->element != nullptr
                    ? expression.type->element
                    : intType(),
                true};
        }
        if (accept(TokenKind::Increment) || accept(TokenKind::Decrement)) {
            const bool decrement =
                tokens_[position_ - 1].kind == TokenKind::Decrement;
            Expression expression = parsePrefix();
            if (!expression.lvalue)
                fail("increment requires a modifiable lvalue");
            emit(makeInstruction(CVM_OP_DUP));
            emit(makeInstruction(CVM_OP_LOAD, vmType(expression.type)));
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 1));
            if (expression.type->kind == TypeKind::Pointer) {
                emit(makeInstruction(
                    CVM_OP_POINTER_INDEX,
                    CVM_TYPE_PTR,
                    decrement
                        ? -static_cast<std::int32_t>(
                              expression.type->element->size)
                        : static_cast<std::int32_t>(
                              expression.type->element->size)));
            } else {
                emit(makeInstruction(
                    decrement ? CVM_OP_SUBTRACT : CVM_OP_ADD,
                    vmType(expression.type)));
            }
            emit(makeInstruction(
                CVM_OP_STORE,
                vmType(expression.type)));
            return {expression.type, false};
        }
        if (accept(TokenKind::Minus)) {
            Expression expression = parsePrefix();
            makeRvalue(expression);
            emit(makeInstruction(
                CVM_OP_NEGATE,
                vmType(expression.type)));
            return {expression.type, false};
        }
        if (accept(TokenKind::Plus))
            return parsePrefix();
        if (accept(TokenKind::LogicalNot)) {
            Expression expression = parsePrefix();
            makeRvalue(expression);
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 0));
            emit(makeInstruction(CVM_OP_COMPARE_EQUAL, CVM_TYPE_I32));
            return {intType(), false};
        }
        if (accept(TokenKind::BitNot)) {
            Expression expression = parsePrefix();
            makeRvalue(expression);
            emit(makeInstruction(CVM_OP_BIT_NOT, CVM_TYPE_I32));
            return {intType(), false};
        }
        if (accept(TokenKind::LeftParen)) {
            Expression expression = parseExpression();
            take(TokenKind::RightParen, "')'");
            return expression;
        }
        if (at(TokenKind::Integer)) {
            const Token token =
                take(TokenKind::Integer, "integer constant");
            std::string spelling = token.text;
            std::string suffix;
            while (!spelling.empty() &&
                   std::strchr("uUlL", spelling.back()) != nullptr) {
                suffix.push_back(
                    static_cast<char>(std::tolower(
                        static_cast<unsigned char>(spelling.back()))));
                spelling.pop_back();
            }
            const bool explicitUnsigned =
                suffix.find('u') != std::string::npos;
            const bool explicitLongLong =
                std::count(suffix.begin(), suffix.end(), 'l') >= 2;
            const bool explicitLong =
                !explicitLongLong &&
                suffix.find('l') != std::string::npos;
            const bool hexadecimal =
                spelling.size() > 2 && spelling[0] == '0' &&
                (spelling[1] == 'x' || spelling[1] == 'X');
            const std::uint64_t bits =
                static_cast<std::uint64_t>(token.integer);
            TypePtr literalType;
            if (explicitLongLong) {
                literalType = scalarType(
                    TypeKind::LongLong,
                    explicitUnsigned);
            } else if (explicitUnsigned) {
                literalType = scalarType(
                    bits <= UINT32_MAX
                        ? TypeKind::Integer
                        : TypeKind::LongLong,
                    true);
            } else if (explicitLong) {
                if (bits <= INT32_MAX) {
                    literalType = scalarType(TypeKind::Long, false);
                } else if (hexadecimal && bits <= UINT32_MAX) {
                    literalType = scalarType(TypeKind::Long, true);
                } else {
                    literalType =
                        scalarType(TypeKind::LongLong, false);
                }
            } else if (bits <= INT32_MAX) {
                literalType = intType();
            } else if (hexadecimal && bits <= UINT32_MAX) {
                literalType =
                    scalarType(TypeKind::Integer, true);
            } else {
                literalType = scalarType(
                    TypeKind::LongLong,
                    hexadecimal && bits > INT64_MAX);
            }
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                vmType(literalType),
                static_cast<std::int32_t>(bits),
                static_cast<std::int32_t>(bits >> 32)));
            return {literalType, false};
        }
        if (at(TokenKind::Floating)) {
            const Token token =
                take(TokenKind::Floating, "floating constant");
            const bool spelledFloat =
                !token.text.empty() &&
                (token.text.back() == 'f' ||
                 token.text.back() == 'F');
            std::uint64_t bits = 0;
            std::memcpy(&bits, &token.floating, sizeof(bits));
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                CVM_TYPE_F64,
                static_cast<std::int32_t>(bits),
                static_cast<std::int32_t>(bits >> 32)));
            return {
                scalarType(
                    spelledFloat ? TypeKind::Float : TypeKind::Double,
                    false),
                false};
        }
        if (at(TokenKind::Character)) {
            const Token token =
                take(TokenKind::Character, "character constant");
            emit(makeInstruction(
                CVM_OP_PUSH_IMMEDIATE,
                CVM_TYPE_I32,
                static_cast<std::int32_t>(token.integer)));
            return {intType(), false};
        }
        if (at(TokenKind::String)) {
            Token token = take(TokenKind::String, "string literal");
            while (at(TokenKind::String)) {
                token.text +=
                    take(TokenKind::String, "string literal").text;
            }
            const std::uint32_t offset =
                static_cast<std::uint32_t>(data_.size());
            data_.insert(data_.end(), token.text.begin(), token.text.end());
            data_.push_back(0);
            emit(makeInstruction(
                CVM_OP_PUSH_CONSTANT_ADDRESS,
                CVM_TYPE_CSTR,
                static_cast<std::int32_t>(offset)));
            return {pointerType(charType(), target_), false};
        }
        if (at(TokenKind::Identifier)) {
            const std::string name =
                take(TokenKind::Identifier, "identifier").text;
            if (name == "NULL") {
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_PTR,
                    0,
                    0));
                return {pointerType(voidType(), target_), false};
            }
            const auto enumValue = enumConstants_.find(name);
            if (enumValue != enumConstants_.end()) {
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I32,
                    static_cast<std::int32_t>(enumValue->second),
                    static_cast<std::int32_t>(
                        static_cast<std::uint64_t>(
                            enumValue->second) >> 32)));
                return {intType(), false};
            }
            const Variable *callable = lookupVariable(name);
            if (callable != nullptr &&
                callable->type->kind ==
                    TypeKind::FunctionPointer &&
                at(TokenKind::LeftParen)) {
                emitAddress(*callable);
                emit(makeInstruction(
                    CVM_OP_LOAD,
                    CVM_TYPE_PTR));
                take(TokenKind::LeftParen, "'('");
                std::vector<TypePtr> argumentTypes;
                std::uint32_t argumentCount = 0;
                if (!at(TokenKind::RightParen)) {
                    for (;;) {
                        Expression argument = parseExpression(1);
                        makeRvalue(argument);
                        TypePtr passedType = argument.type;
                        if (argumentCount <
                            callable->type->parameterTypes.size()) {
                            TypePtr parameter =
                                callable->type->parameterTypes[
                                    argumentCount];
                            emitConversion(argument.type, parameter);
                            passedType = parameter;
                        } else if (callable->type->variadic) {
                            if (argument.type->kind ==
                                TypeKind::Float) {
                                passedType = scalarType(
                                    TypeKind::Double,
                                    false);
                                emitConversion(
                                    argument.type,
                                    passedType);
                            } else if (
                                argument.type->kind ==
                                    TypeKind::Character ||
                                argument.type->kind ==
                                    TypeKind::Short) {
                                passedType = intType();
                                emitConversion(
                                    argument.type,
                                    passedType);
                            }
                        }
                        argumentTypes.push_back(passedType);
                        ++argumentCount;
                        if (!accept(TokenKind::Comma))
                            break;
                    }
                }
                take(TokenKind::RightParen, "')'");
                const std::size_t fixed =
                    callable->type->parameterTypes.size();
                if ((!callable->type->variadic &&
                     argumentCount != fixed) ||
                    (callable->type->variadic &&
                     argumentCount < fixed)) {
                    fail(
                        "argument count mismatch calling function pointer '" +
                        name + "'");
                }
                CallFixup fixup;
                fixup.instruction =
                    static_cast<std::uint32_t>(
                        function_->code.size());
                fixup.function = name;
                fixup.argumentTypes = std::move(argumentTypes);
                fixup.nativeIndirect = true;
                fixup.callingConvention =
                    callable->type->callingConvention;
                function_->calls.push_back(std::move(fixup));
                emit(makeInstruction(
                    CVM_OP_CALL_NATIVE_INDIRECT,
                    vmType(callable->type->returnType),
                    0,
                    static_cast<std::int32_t>(argumentCount)));
                return {callable->type->returnType, false};
            }
            if (accept(TokenKind::LeftParen)) {
                std::uint32_t argumentCount = 0;
                std::vector<TypePtr> argumentTypes;
                const auto prototypeFound = prototypes_.find(name);
                const FunctionPrototype *prototype =
                    prototypeFound == prototypes_.end()
                        ? nullptr
                        : &prototypeFound->second;
                if (!at(TokenKind::RightParen)) {
                    for (;;) {
                        Expression argument = parseExpression(1);
                        makeRvalue(argument);
                        TypePtr passedType = argument.type;
                        TypePtr parameterType =
                            findFunctionParameterType(
                                name,
                                argumentCount);
                        if (parameterType != nullptr) {
                            emitConversion(argument.type, parameterType);
                            passedType = parameterType;
                        } else if (
                            name == "printf" ||
                            (prototype != nullptr &&
                             prototype->variadic &&
                             argumentCount >=
                                 prototype->parameterTypes.size())) {
                            if (argument.type->kind == TypeKind::Float) {
                                passedType =
                                    scalarType(TypeKind::Double, false);
                                emitConversion(argument.type, passedType);
                            } else if (
                                argument.type->kind == TypeKind::Character ||
                                argument.type->kind == TypeKind::Short) {
                                passedType = intType();
                                emitConversion(argument.type, passedType);
                            }
                        }
                        argumentTypes.push_back(passedType);
                        ++argumentCount;
                        if (!accept(TokenKind::Comma))
                            break;
                    }
                }
                take(TokenKind::RightParen, "')'");
                if (prototype != nullptr && prototype->native) {
                    const std::size_t fixed =
                        prototype->parameterTypes.size();
                    if ((!prototype->variadic &&
                         argumentCount != fixed) ||
                        (prototype->variadic &&
                         argumentCount < fixed)) {
                        fail(
                            "argument count mismatch calling native function '" +
                            name + "'");
                    }
                }
                CallFixup fixup;
                fixup.instruction =
                    static_cast<std::uint32_t>(function_->code.size());
                fixup.function = name;
                fixup.argumentTypes = std::move(argumentTypes);
                if (prototype != nullptr && prototype->native) {
                    fixup.nativeDeclared = true;
                    fixup.library = prototype->library;
                    fixup.symbol = prototype->symbol;
                    fixup.callingConvention =
                        prototype->callingConvention;
                }
                function_->calls.push_back(std::move(fixup));
                emit(makeInstruction(
                    CVM_OP_CALL,
                    vmType(findFunctionReturnType(name)),
                    0,
                    static_cast<std::int32_t>(argumentCount)));
                return {findFunctionReturnType(name), false};
            }

            if (lookupVariable(name) == nullptr &&
                at(TokenKind::Assign) &&
                position_ + 1 < tokens_.size()) {
                TypePtr inferred;
                const TokenKind firstValue =
                    tokens_[position_ + 1].kind;
                if (firstValue == TokenKind::Floating)
                    inferred = scalarType(TypeKind::Double, false);
                else if (firstValue == TokenKind::String)
                    inferred = pointerType(charType(), target_);
                else if (firstValue == TokenKind::BitAnd) {
                    const Variable *pointed = nullptr;
                    if (position_ + 2 < tokens_.size() &&
                        tokens_[position_ + 2].kind ==
                            TokenKind::Identifier) {
                        pointed = lookupVariable(
                            tokens_[position_ + 2].text);
                    }
                    inferred = pointerType(
                        pointed != nullptr
                            ? pointed->type
                            : voidType(),
                        target_);
                }
                else
                    inferred =
                        scalarType(TypeKind::LongLong, false);
                function_->localBytes =
                    alignStorage(
                        function_->localBytes,
                        inferred->alignment);
                function_->localAlignment = std::max(
                    function_->localAlignment,
                    inferred->alignment);
                const std::uint32_t offset = function_->localBytes;
                function_->localBytes += storageSize(inferred);
                localScopes_.back().emplace(
                    name,
                    Variable{offset, false, inferred});
            }
            const Variable &variable = findVariable(name);
            emitAddress(variable);
            Expression result{variable.type, true};
            for (;;) {
                if (accept(TokenKind::LeftBracket)) {
                    TypePtr element;
                    if (result.type->kind == TypeKind::Array) {
                        element = result.type->element;
                        result.type = pointerType(element, target_);
                        result.lvalue = false;
                    } else if (
                        result.type->kind == TypeKind::Pointer) {
                        element = result.type->element;
                        makeRvalue(result);
                    } else {
                        fail("subscript requires an array or pointer");
                    }
                    Expression index = parseExpression();
                    makeRvalue(index);
                    take(TokenKind::RightBracket, "']'");
                    emit(makeInstruction(
                        CVM_OP_POINTER_INDEX,
                        CVM_TYPE_PTR,
                        static_cast<std::int32_t>(element->size)));
                    result = {element, true};
                    continue;
                }
                const bool arrow = accept(TokenKind::Arrow);
                if (!arrow && !accept(TokenKind::Dot))
                    break;
                TypePtr aggregate = result.type;
                if (arrow) {
                    makeRvalue(result);
                    if (aggregate->kind != TypeKind::Pointer)
                        fail("'->' requires a pointer");
                    aggregate = aggregate->element;
                }
                if (aggregate->kind != TypeKind::Structure &&
                    aggregate->kind != TypeKind::Union)
                    fail("member access requires an aggregate");
                const std::string fieldName =
                    take(TokenKind::Identifier, "field name").text;
                const auto field = std::find_if(
                    aggregate->fields.begin(),
                    aggregate->fields.end(),
                    [&](const CField &candidate) {
                        return candidate.name == fieldName;
                    });
                if (field == aggregate->fields.end())
                    fail("unknown aggregate field '" + fieldName + "'");
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE,
                    CVM_TYPE_I32,
                    static_cast<std::int32_t>(field->offset)));
                emit(makeInstruction(CVM_OP_POINTER_ADD, CVM_TYPE_PTR));
                result = {field->type, true};
            }
            if (accept(TokenKind::Increment) ||
                accept(TokenKind::Decrement)) {
                const bool decrement =
                    tokens_[position_ - 1].kind == TokenKind::Decrement;
                emit(makeInstruction(CVM_OP_DUP));
                emit(makeInstruction(
                    CVM_OP_LOAD,
                    vmType(result.type)));
                emit(makeInstruction(CVM_OP_SWAP));
                emit(makeInstruction(CVM_OP_DUP));
                emit(makeInstruction(
                    CVM_OP_LOAD,
                    vmType(result.type)));
                emit(makeInstruction(
                    CVM_OP_PUSH_IMMEDIATE, CVM_TYPE_I32, 1));
                if (result.type->kind == TypeKind::Pointer) {
                    emit(makeInstruction(
                        CVM_OP_POINTER_INDEX,
                        CVM_TYPE_PTR,
                        decrement
                            ? -static_cast<std::int32_t>(
                                  result.type->element->size)
                            : static_cast<std::int32_t>(
                                  result.type->element->size)));
                } else {
                    emit(makeInstruction(
                        decrement ? CVM_OP_SUBTRACT : CVM_OP_ADD,
                        vmType(result.type)));
                }
                emit(makeInstruction(
                    CVM_OP_STORE,
                    vmType(result.type)));
                emit(makeInstruction(CVM_OP_POP));
                return {result.type, false};
            }
            return result;
        }
        fail("expected an expression");
    }
};

class StringTable {
public:
    std::uint32_t add(const std::string &value)
    {
        const auto found = offsets_.find(value);
        if (found != offsets_.end())
            return found->second;
        const auto offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        bytes_.push_back('\0');
        offsets_.emplace(value, offset);
        return offset;
    }
    const std::vector<char> &bytes() const { return bytes_; }
private:
    std::vector<char> bytes_;
    std::unordered_map<std::string, std::uint32_t> offsets_;
};

template<typename T>
void append(std::vector<std::uint8_t> &output, const T *data, std::size_t count)
{
    if (count == 0)
        return;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(data);
    output.insert(output.end(), bytes, bytes + sizeof(T) * count);
}

void writePackage(
    CompilationUnit unit,
    const std::string &outputPath,
    const TargetDataModel &target)
{
    std::vector<Function> functions = std::move(unit.functions);
    std::unordered_map<std::string, std::uint32_t> functionIndices;
    StringTable strings;
    std::vector<CvmFunction> descriptors;
    std::vector<CvmParameter> parameters;
    std::vector<CvmInstruction> code;
    std::vector<CvmNativeImport> imports;
    std::vector<CvmNativeSignature> signatures;
    std::vector<std::uint8_t> signatureParameters;
    std::uint32_t entry = 0;

    for (std::uint32_t i = 0; i < functions.size(); ++i) {
        if (!functionIndices.emplace(functions[i].name, i).second)
            throw std::runtime_error("duplicate function '" +
                                     functions[i].name + "'");
    }

    for (auto &function : functions) {
        for (const auto &fixup : function.calls) {
            if (fixup.nativeIndirect) {
                CvmNativeSignature signature{};
                signature.first_parameter_type =
                    static_cast<std::uint32_t>(
                        signatureParameters.size());
                signature.parameter_count =
                    static_cast<std::uint16_t>(
                        fixup.argumentTypes.size());
                signature.return_type =
                    function.code[fixup.instruction].type;
                signature.calling_convention =
                    static_cast<std::uint8_t>(
                        fixup.callingConvention);
                for (const TypePtr &type : fixup.argumentTypes) {
                    signatureParameters.push_back(
                        static_cast<std::uint8_t>(vmType(type)));
                }
                function.code[fixup.instruction].opcode =
                    CVM_OP_CALL_NATIVE_INDIRECT;
                function.code[fixup.instruction].a =
                    static_cast<std::int32_t>(signatures.size());
                signatures.push_back(signature);
                continue;
            }
            const auto found = functionIndices.find(fixup.function);
            if (found == functionIndices.end()) {
                CvmNativeSignature signature{};
                CvmNativeImport import{};
                signature.first_parameter_type =
                    static_cast<std::uint32_t>(signatureParameters.size());
                signature.parameter_count =
                    static_cast<std::uint16_t>(fixup.argumentTypes.size());
                signature.return_type =
                    function.code[fixup.instruction].type;
                signature.calling_convention =
                    static_cast<std::uint8_t>(
                        fixup.nativeDeclared
                            ? fixup.callingConvention
                            : CVM_CALL_CDECL);
                for (const TypePtr &type : fixup.argumentTypes)
                    signatureParameters.push_back(
                        static_cast<std::uint8_t>(vmType(type)));
                import.library_string = strings.add(
                    fixup.nativeDeclared ? fixup.library : "PICOC");
                import.symbol_string = strings.add(
                    fixup.nativeDeclared ? fixup.symbol : fixup.function);
                import.signature_index =
                    static_cast<std::uint32_t>(signatures.size());
                signatures.push_back(signature);
                function.code[fixup.instruction].opcode = CVM_OP_CALL_IMPORT;
                function.code[fixup.instruction].a =
                    static_cast<std::int32_t>(imports.size());
                imports.push_back(import);
                continue;
            }
            function.code[fixup.instruction].a =
                static_cast<std::int32_t>(found->second);
            function.code[fixup.instruction].type =
                static_cast<std::uint8_t>(
                    vmType(functions[found->second].returnType));
            if (function.code[fixup.instruction].b !=
                static_cast<std::int32_t>(
                    functions[found->second].parameters.size())) {
                throw std::runtime_error(
                    "argument count mismatch calling '" + fixup.function + "'");
            }
        }

        CvmFunction descriptor{};
        descriptor.name_string = strings.add(function.name);
        descriptor.first_instruction =
            static_cast<std::uint32_t>(code.size());
        descriptor.instruction_count =
            static_cast<std::uint32_t>(function.code.size());
        descriptor.first_parameter =
            static_cast<std::uint32_t>(parameters.size());
        descriptor.parameter_count =
            static_cast<std::uint16_t>(function.parameters.size());
        descriptor.return_type =
            static_cast<std::uint8_t>(vmType(function.returnType));
        descriptor.local_bytes = function.localBytes;
        descriptor.local_alignment =
            static_cast<std::uint16_t>(function.localAlignment);
        descriptor.maximum_stack_cells = 64;

        for (CvmInstruction &instruction : function.code) {
            if (instruction.opcode == CVM_OP_JUMP ||
                instruction.opcode == CVM_OP_JUMP_IF_ZERO ||
                instruction.opcode == CVM_OP_JUMP_IF_NONZERO) {
                instruction.a +=
                    static_cast<std::int32_t>(
                        descriptor.first_instruction);
            }
        }

        for (auto parameter : function.parameters) {
            parameter.second.name_string = strings.add(parameter.first);
            parameters.push_back(parameter.second);
        }
        descriptors.push_back(descriptor);
        code.insert(code.end(), function.code.begin(), function.code.end());
    }

    constexpr std::uint32_t sectionCount = 8;
    CvmPackageHeader header{};
    header.magic = CVM_MAGIC;
    header.format_major = CVM_FORMAT_MAJOR;
    header.format_minor = CVM_FORMAT_MINOR;
    header.target_arch = static_cast<std::uint8_t>(target.architecture);
    header.pointer_size = static_cast<std::uint8_t>(target.pointerSize);
    header.endian = 1;
    header.profile = CVM_PROFILE_PICOC_COMPAT;
    if (!imports.empty())
        header.features |= CVM_FEATURE_NATIVE_IMPORTS;
    if (std::any_of(
            code.begin(),
            code.end(),
            [](const CvmInstruction &instruction) {
                return instruction.opcode ==
                    CVM_OP_CALL_NATIVE_INDIRECT;
            })) {
        header.features |= CVM_FEATURE_NATIVE_INDIRECT;
    }
    header.section_count = sectionCount;
    header.entry_function = entry;
    header.required_stack_cells = 256;
    header.required_call_depth = 128;
    header.global_bytes = unit.globalBytes;

    std::vector<CvmSectionHeader> sections(sectionCount);
    std::uint32_t offset = sizeof(header) +
        sectionCount * sizeof(CvmSectionHeader);
    auto defineSection = [&](std::size_t index,
                             CvmSectionKind kind,
                             std::uint32_t size,
                             std::uint32_t count,
                             std::uint32_t entrySize) {
        sections[index].kind = static_cast<std::uint16_t>(kind);
        sections[index].flags = CVM_SECTION_REQUIRED;
        sections[index].offset = offset;
        sections[index].size = size;
        sections[index].count = count;
        sections[index].entry_size = entrySize;
        offset += size;
    };
    defineSection(
        0, CVM_SECTION_STRINGS,
        static_cast<std::uint32_t>(strings.bytes().size()), 0, 0);
    defineSection(
        1, CVM_SECTION_DATA,
        static_cast<std::uint32_t>(unit.data.size()), 0, 0);
    defineSection(
        2, CVM_SECTION_PARAMETERS,
        static_cast<std::uint32_t>(parameters.size() * sizeof(CvmParameter)),
        static_cast<std::uint32_t>(parameters.size()),
        sizeof(CvmParameter));
    defineSection(
        3, CVM_SECTION_FUNCTIONS,
        static_cast<std::uint32_t>(descriptors.size() * sizeof(CvmFunction)),
        static_cast<std::uint32_t>(descriptors.size()),
        sizeof(CvmFunction));
    defineSection(
        4, CVM_SECTION_CODE,
        static_cast<std::uint32_t>(code.size() * sizeof(CvmInstruction)),
        static_cast<std::uint32_t>(code.size()),
        sizeof(CvmInstruction));
    defineSection(
        5, CVM_SECTION_IMPORTS,
        static_cast<std::uint32_t>(
            imports.size() * sizeof(CvmNativeImport)),
        static_cast<std::uint32_t>(imports.size()),
        sizeof(CvmNativeImport));
    defineSection(
        6, CVM_SECTION_SIGNATURES,
        static_cast<std::uint32_t>(
            signatures.size() * sizeof(CvmNativeSignature)),
        static_cast<std::uint32_t>(signatures.size()),
        sizeof(CvmNativeSignature));
    defineSection(
        7, CVM_SECTION_SIGNATURE_PARAMETERS,
        static_cast<std::uint32_t>(signatureParameters.size()),
        static_cast<std::uint32_t>(signatureParameters.size()),
        sizeof(std::uint8_t));
    header.package_size = offset;

    std::vector<std::uint8_t> package;
    package.reserve(header.package_size);
    append(package, &header, 1);
    append(package, sections.data(), sections.size());
    append(package, strings.bytes().data(), strings.bytes().size());
    append(package, unit.data.data(), unit.data.size());
    append(package, parameters.data(), parameters.size());
    append(package, descriptors.data(), descriptors.size());
    append(package, code.data(), code.size());
    append(package, imports.data(), imports.size());
    append(package, signatures.data(), signatures.size());
    append(
        package,
        signatureParameters.data(),
        signatureParameters.size());

    std::ofstream output(outputPath, std::ios::binary);
    if (!output)
        throw std::runtime_error("cannot create output file");
    output.write(
        reinterpret_cast<const char *>(package.data()),
        static_cast<std::streamsize>(package.size()));
    if (!output)
        throw std::runtime_error("failed to write output file");
}

} // namespace

int main(int argc, char **argv)
{
    TargetDataModel target = defaultTargetDataModel();
    std::vector<std::filesystem::path> includePaths;
    int argument = 1;
    while (argument < argc) {
        if (std::strcmp(argv[argument], "--target") == 0) {
            if (argument + 1 >= argc) {
                std::fprintf(stderr, "--target requires x86 or x64\n");
                return 2;
            }
            const char *name = argv[argument + 1];
            if (std::strcmp(name, "x86") == 0 ||
                std::strcmp(name, "win32") == 0) {
                target = targetDataModel(CVM_ARCH_X86);
            } else if (
                std::strcmp(name, "x64") == 0 ||
                std::strcmp(name, "win64") == 0) {
                target = targetDataModel(CVM_ARCH_X64);
            } else {
                std::fprintf(
                    stderr,
                    "unknown target '%s'; expected x86 or x64\n",
                    name);
                return 2;
            }
            argument += 2;
            continue;
        }
        if (std::strcmp(argv[argument], "-I") == 0) {
            if (argument + 1 >= argc) {
                std::fprintf(stderr, "-I requires a directory\n");
                return 2;
            }
            includePaths.emplace_back(argv[argument + 1]);
            argument += 2;
            continue;
        }
        if (std::strncmp(argv[argument], "-I", 2) == 0 &&
            argv[argument][2] != '\0') {
            includePaths.emplace_back(argv[argument] + 2);
            ++argument;
            continue;
        }
        break;
    }
    if (argc - argument != 2) {
        std::fprintf(
            stderr,
            "usage: cvmc [--target x86|x64] [-I directory] "
            "<input.c> <output.cvm>\n");
        return 2;
    }
    try {
        Preprocessor preprocessor(includePaths, target.architecture);
        const std::string preprocessed =
            preprocessor.process(argv[argument]);
        if (std::strcmp(argv[argument + 1], "-E") == 0) {
            std::fwrite(
                preprocessed.data(),
                1,
                preprocessed.size(),
                stdout);
            return 0;
        }
        Lexer lexer(preprocessed, argv[argument]);
        Parser parser(lexer.scan(), argv[argument], target);
        writePackage(parser.parse(), argv[argument + 1], target);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
