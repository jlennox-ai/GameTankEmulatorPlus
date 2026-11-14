#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The variables in a build's variables.dbg (written from its DWARF debug info by the
// project's tools/llvm_variables.py): which are in scope at a PC, where each one is there (a
// register, the compiler's zero-page registers, the C stack, an address, or a constant), and
// how to show its value with its type. Used by the code stepper's source view.
class VariableMap {
public:
    struct Registers {
        uint8_t a;
        uint8_t x;
        uint8_t y;
    };
    // Reads emulated memory without side effects.
    typedef std::function<uint8_t(uint16_t)> MemoryReader;
    // One step of a hovered expression: the separator before the name ('\0' for the variable
    // itself, '.' for a member, '>' for "->", '[' for an index), and the name (an index's text).
    typedef std::vector<std::pair<char, std::string>> Chain;

    static VariableMap* singleton;

    explicit VariableMap(const std::string& path);

    // Whether name is a variable (local, parameter or global) in scope at pc.
    bool IsVisible(const std::string& name, uint16_t pc) const;
    // The chain's value at pc, for a tooltip: "<expression> = <value>  <type>, <where>", with a
    // struct's fields on the lines after. Empty if the chain's first name isn't in scope.
    std::string Describe(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read) const;
    // Where the chain's value is in memory at pc: its address and size in bytes. False if it
    // isn't in scope, or isn't in memory (a register, a bit-field, optimized out).
    bool Locate(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read,
        uint16_t& address, uint32_t& size) const;
    // An array index's value at pc: a number (decimal, or hex with 0x) or a variable in scope.
    // False for anything else (an expression, a macro).
    bool ResolveIndex(const std::string& text, uint16_t pc, const Registers& registers, const MemoryReader& read,
        int64_t& index) const;

private:
    struct Member {
        std::string name;
        uint32_t type;
        uint32_t bitOffset; // from the start of the struct
        uint32_t bitSize;   // 0: not a bit-field
    };
    struct Type {
        std::string kind; // base, pointer, typedef, struct, union, enum, array, function
        std::string name;
        uint32_t size = 0;
        uint32_t target = 0; // pointer, typedef, enum (underlying), array (element); 0: none
        std::string encoding;
        std::vector<Member> members;
        std::vector<std::pair<int64_t, std::string>> enumerators;
        std::vector<uint32_t> counts; // array dimensions
    };
    struct Location {
        uint32_t low;
        uint32_t high;
        std::vector<std::string> operations;
    };
    struct Variable {
        std::string name;
        uint32_t type;
        uint32_t scope;
        std::vector<Location> locations;
    };
    struct Scope {
        uint32_t parent;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
    };
    // A value read for display: its bytes (little-endian), and its address when it's in memory.
    struct Value {
        bool available = false;
        std::vector<uint8_t> bytes;
        int32_t address = -1;
        std::string where;
    };

    // A chain followed from its variable through its members.
    struct ChainValue {
        std::string expression; // the chain as C: "block->Color"
        std::string error;      // why it has no value (starting with the line break or spacing to show it with)
        Value value;
        uint32_t type = 0;
        // Part of a multi-dimensional array: its dimensions left (type is the element type).
        std::vector<uint32_t> arrayCounts;
        std::string where;
    };

    uint16_t registerBase = 0;
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, Scope> scopes;
    std::vector<Variable> variables;

    const Variable* Find(const std::string& name, uint16_t pc) const;
    // False if the chain's first name isn't in scope at pc.
    bool Follow(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read, ChainValue& result) const;
    bool InScope(uint32_t scope, uint16_t pc, int& depth) const;
    const Type* Resolve(uint32_t type) const;
    uint32_t SizeOf(uint32_t type) const;
    std::string TypeName(uint32_t type) const;
    std::string ArrayTypeName(uint32_t element, const std::vector<uint32_t>& counts) const;
    std::string FormatArray(uint32_t element, const std::vector<uint32_t>& counts, const std::vector<uint8_t>& bytes) const;
    Value Evaluate(const Variable& variable, uint16_t pc, const Registers& registers, const MemoryReader& read) const;
    int64_t RegisterValue(const std::string& name, const Registers& registers, const MemoryReader& read) const;
    std::vector<uint8_t> RegisterBytes(const std::string& name, uint32_t size, const Registers& registers, const MemoryReader& read) const;
    std::string Format(uint32_t type, const std::vector<uint8_t>& bytes, const MemoryReader& read, int depth) const;
};
