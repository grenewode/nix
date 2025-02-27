#include <limits>
#include <unordered_set>

#include "print.hh"
#include "ansicolor.hh"
#include "signals.hh"
#include "store-api.hh"
#include "terminal.hh"
#include "english.hh"
#include "eval.hh"

namespace nix {

void printElided(
    std::ostream & output,
    unsigned int value,
    const std::string_view single,
    const std::string_view plural,
    bool ansiColors)
{
    if (ansiColors)
        output << ANSI_FAINT;
    output << "«";
    pluralize(output, value, single, plural);
    output << " elided»";
    if (ansiColors)
        output << ANSI_NORMAL;
}


std::ostream &
printLiteralString(std::ostream & str, const std::string_view string, size_t maxLength, bool ansiColors)
{
    size_t charsPrinted = 0;
    if (ansiColors)
        str << ANSI_MAGENTA;
    str << "\"";
    for (auto i = string.begin(); i != string.end(); ++i) {
        if (charsPrinted >= maxLength) {
            str << "\" ";
            printElided(str, string.length() - charsPrinted, "byte", "bytes", ansiColors);
            return str;
        }

        if (*i == '\"' || *i == '\\') str << "\\" << *i;
        else if (*i == '\n') str << "\\n";
        else if (*i == '\r') str << "\\r";
        else if (*i == '\t') str << "\\t";
        else if (*i == '$' && *(i+1) == '{') str << "\\" << *i;
        else str << *i;
        charsPrinted++;
    }
    str << "\"";
    if (ansiColors)
        str << ANSI_NORMAL;
    return str;
}

std::ostream &
printLiteralString(std::ostream & str, const std::string_view string)
{
    return printLiteralString(str, string, std::numeric_limits<size_t>::max(), false);
}

std::ostream &
printLiteralBool(std::ostream & str, bool boolean)
{
    str << (boolean ? "true" : "false");
    return str;
}

// Returns `true' is a string is a reserved keyword which requires quotation
// when printing attribute set field names.
//
// This list should generally be kept in sync with `./lexer.l'.
// You can test if a keyword needs to be added by running:
//   $ nix eval --expr '{ <KEYWORD> = 1; }'
// For example `or' doesn't need to be quoted.
bool isReservedKeyword(const std::string_view str)
{
    static const std::unordered_set<std::string_view> reservedKeywords = {
        "if", "then", "else", "assert", "with", "let", "in", "rec", "inherit"
    };
    return reservedKeywords.contains(str);
}

std::ostream &
printIdentifier(std::ostream & str, std::string_view s) {
    if (s.empty())
        str << "\"\"";
    else if (isReservedKeyword(s))
        str << '"' << s << '"';
    else {
        char c = s[0];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
            printLiteralString(str, s);
            return str;
        }
        for (auto c : s)
            if (!((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '\'' || c == '-')) {
                printLiteralString(str, s);
                return str;
            }
        str << s;
    }
    return str;
}

static bool isVarName(std::string_view s)
{
    if (s.size() == 0) return false;
    if (isReservedKeyword(s)) return false;
    char c = s[0];
    if ((c >= '0' && c <= '9') || c == '-' || c == '\'') return false;
    for (auto & i : s)
        if (!((i >= 'a' && i <= 'z') ||
              (i >= 'A' && i <= 'Z') ||
              (i >= '0' && i <= '9') ||
              i == '_' || i == '-' || i == '\''))
            return false;
    return true;
}

std::ostream &
printAttributeName(std::ostream & str, std::string_view name) {
    if (isVarName(name))
        str << name;
    else
        printLiteralString(str, name);
    return str;
}

bool isImportantAttrName(const std::string& attrName)
{
    return attrName == "type" || attrName == "_type";
}

typedef std::pair<std::string, Value *> AttrPair;

struct ImportantFirstAttrNameCmp
{

    bool operator()(const AttrPair& lhs, const AttrPair& rhs) const
    {
        auto lhsIsImportant = isImportantAttrName(lhs.first);
        auto rhsIsImportant = isImportantAttrName(rhs.first);
        return std::forward_as_tuple(!lhsIsImportant, lhs.first)
            < std::forward_as_tuple(!rhsIsImportant, rhs.first);
    }
};

typedef std::set<Value *> ValuesSeen;

class Printer
{
private:
    std::ostream & output;
    EvalState & state;
    PrintOptions options;
    std::optional<ValuesSeen> seen;
    size_t attrsPrinted = 0;
    size_t listItemsPrinted = 0;

    void printRepeated()
    {
        if (options.ansiColors)
            output << ANSI_MAGENTA;
        output << "«repeated»";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printNullptr()
    {
        if (options.ansiColors)
            output << ANSI_MAGENTA;
        output << "«nullptr»";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printElided(unsigned int value, const std::string_view single, const std::string_view plural)
    {
        ::nix::printElided(output, value, single, plural, options.ansiColors);
    }

    void printInt(Value & v)
    {
        if (options.ansiColors)
            output << ANSI_CYAN;
        output << v.integer;
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printFloat(Value & v)
    {
        if (options.ansiColors)
            output << ANSI_CYAN;
        output << v.fpoint;
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printBool(Value & v)
    {
        if (options.ansiColors)
            output << ANSI_CYAN;
        printLiteralBool(output, v.boolean);
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printString(Value & v)
    {
        printLiteralString(output, v.string_view(), options.maxStringLength, options.ansiColors);
    }

    void printPath(Value & v)
    {
        if (options.ansiColors)
            output << ANSI_GREEN;
        output << v.path().to_string(); // !!! escaping?
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printNull()
    {
        if (options.ansiColors)
            output << ANSI_CYAN;
        output << "null";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printDerivation(Value & v)
    {
        try {
            Bindings::iterator i = v.attrs->find(state.sDrvPath);
            NixStringContext context;
            std::string storePath;
            if (i != v.attrs->end())
                storePath = state.store->printStorePath(state.coerceToStorePath(i->pos, *i->value, context, "while evaluating the drvPath of a derivation"));

            if (options.ansiColors)
                output << ANSI_GREEN;
            output << "«derivation";
            if (!storePath.empty()) {
                output << " " << storePath;
            }
            output << "»";
            if (options.ansiColors)
                output << ANSI_NORMAL;
        } catch (BaseError & e) {
            printError_(e);
        }
    }

    void printAttrs(Value & v, size_t depth)
    {
        if (seen && !seen->insert(&v).second) {
            printRepeated();
            return;
        }

        if (options.force && options.derivationPaths && state.isDerivation(v)) {
            printDerivation(v);
        } else if (depth < options.maxDepth) {
            output << "{ ";

            std::vector<std::pair<std::string, Value *>> sorted;
            for (auto & i : *v.attrs)
                sorted.emplace_back(std::pair(state.symbols[i.name], i.value));

            if (options.maxAttrs == std::numeric_limits<size_t>::max())
                std::sort(sorted.begin(), sorted.end());
            else
                std::sort(sorted.begin(), sorted.end(), ImportantFirstAttrNameCmp());

            for (auto & i : sorted) {
                if (attrsPrinted >= options.maxAttrs) {
                    printElided(sorted.size() - attrsPrinted, "attribute", "attributes");
                    break;
                }

                printAttributeName(output, i.first);
                output << " = ";
                print(*i.second, depth + 1);
                output << "; ";
                attrsPrinted++;
            }

            output << "}";
        } else
            output << "{ ... }";
    }

    void printList(Value & v, size_t depth)
    {
        if (seen && v.listSize() && !seen->insert(&v).second) {
            printRepeated();
            return;
        }

        output << "[ ";
        if (depth < options.maxDepth) {
            for (auto elem : v.listItems()) {
                if (listItemsPrinted >= options.maxListItems) {
                    printElided(v.listSize() - listItemsPrinted, "item", "items");
                    break;
                }

                if (elem) {
                    print(*elem, depth + 1);
                } else {
                    printNullptr();
                }
                output << " ";
                listItemsPrinted++;
            }
        }
        else
            output << "... ";
        output << "]";
    }

    void printFunction(Value & v)
    {
        if (options.ansiColors)
            output << ANSI_BLUE;
        output << "«";

        if (v.isLambda()) {
            output << "lambda";
            if (v.lambda.fun) {
                if (v.lambda.fun->name) {
                    output << " " << state.symbols[v.lambda.fun->name];
                }

                std::ostringstream s;
                s << state.positions[v.lambda.fun->pos];
                output << " @ " << filterANSIEscapes(s.str());
            }
        } else if (v.isPrimOp()) {
            if (v.primOp)
                output << *v.primOp;
            else
                output << "primop";
        } else if (v.isPrimOpApp()) {
            output << "partially applied ";
            auto primOp = v.primOpAppPrimOp();
            if (primOp)
                output << *primOp;
            else
                output << "primop";
        } else {
            abort();
        }

        output << "»";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printThunk(Value & v)
    {
        if (v.isBlackhole()) {
            // Although we know for sure that it's going to be an infinite recursion
            // when this value is accessed _in the current context_, it's likely
            // that the user will misinterpret a simpler «infinite recursion» output
            // as a definitive statement about the value, while in fact it may be
            // a valid value after `builtins.trace` and perhaps some other steps
            // have completed.
            if (options.ansiColors)
                output << ANSI_RED;
            output << "«potential infinite recursion»";
            if (options.ansiColors)
                output << ANSI_NORMAL;
        } else if (v.isThunk() || v.isApp()) {
            if (options.ansiColors)
                    output << ANSI_MAGENTA;
            output << "«thunk»";
            if (options.ansiColors)
                    output << ANSI_NORMAL;
        } else {
            abort();
        }
    }

    void printExternal(Value & v)
    {
        v.external->print(output);
    }

    void printUnknown()
    {
        if (options.ansiColors)
            output << ANSI_RED;
        output << "«unknown»";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void printError_(BaseError & e)
    {
        if (options.ansiColors)
            output << ANSI_RED;
        output << "«" << e.msg() << "»";
        if (options.ansiColors)
            output << ANSI_NORMAL;
    }

    void print(Value & v, size_t depth)
    {
        output.flush();
        checkInterrupt();

        if (options.force) {
            try {
                state.forceValue(v, v.determinePos(noPos));
            } catch (BaseError & e) {
                printError_(e);
                return;
            }
        }

        switch (v.type()) {

        case nInt:
            printInt(v);
            break;

        case nFloat:
            printFloat(v);
            break;

        case nBool:
            printBool(v);
            break;

        case nString:
            printString(v);
            break;

        case nPath:
            printPath(v);
            break;

        case nNull:
            printNull();
            break;

        case nAttrs:
            printAttrs(v, depth);
            break;

        case nList:
            printList(v, depth);
            break;

        case nFunction:
            printFunction(v);
            break;

        case nThunk:
            printThunk(v);
            break;

        case nExternal:
            printExternal(v);
            break;

        default:
            printUnknown();
            break;
        }
    }

public:
    Printer(std::ostream & output, EvalState & state, PrintOptions options)
        : output(output), state(state), options(options) { }

    void print(Value & v)
    {
        attrsPrinted = 0;
        listItemsPrinted = 0;

        if (options.trackRepeated) {
            seen.emplace();
        } else {
            seen.reset();
        }

        ValuesSeen seen;
        print(v, 0);
    }
};

void printValue(EvalState & state, std::ostream & output, Value & v, PrintOptions options)
{
    Printer(output, state, options).print(v);
}

std::ostream & operator<<(std::ostream & output, const ValuePrinter & printer)
{
    printValue(printer.state, output, printer.value, printer.options);
    return output;
}

}
