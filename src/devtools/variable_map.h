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

    // What a byte of memory is, by the variables at fixed addresses (globals and static locals,
    // including ones the optimizer split into pieces at different addresses), for the memory
    // browser: the innermost part of one it's in, and the struct around that.
    struct MemoryOwner {
        // The innermost variable, member or element the byte is in: "g_well.Blocks[2][3].Timer".
        // Bit-fields sharing the byte are all named: "g_well.Blocks[2][3] (State, Color)".
        std::string part;
        // The innermost struct around it, or the part itself when it's in none.
        std::string container;
        std::string containerType;
        // Where the container is: "$0200-$0233", or that the optimizer spread it out.
        std::string containerWhere;
        // The container's value a line at a time, each marked when it's (in) the part.
        std::vector<std::pair<std::string, bool>> lines;
        // Which bytes of the variable they are, for ByteRole.
        size_t variable = 0;
        uint32_t containerOffset = 0;
        uint32_t containerSize = 0;
        uint32_t partOffset = 0;
        uint32_t partSize = 0;
    };
    // False if no variable at a fixed address covers the byte.
    bool FindOwner(uint16_t address, const MemoryReader& read, MemoryOwner& owner) const;
    enum class ByteRole { None, Container, Part };
    // What another byte is to an owner found by FindOwner: in its part, elsewhere in its
    // container, or neither.
    ByteRole RoleOf(const MemoryOwner& owner, uint16_t address) const;

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

    // A run of a fixed-address variable's bytes in memory: the whole variable, or one piece of
    // one the optimizer split up.
    struct MemoryPiece {
        uint16_t address;
        uint32_t size;
        uint32_t offset; // into the variable
    };
    // One step down into a fixed-address variable: the variable itself, one of its structs'
    // members, or one of its arrays' elements.
    struct Level {
        std::string path;    // "g_well.Blocks[2][3]"
        uint32_t type;       // 0: bit-fields (or a union's members) sharing a byte, named in path
        uint32_t start;      // offset into the variable
        uint32_t size;
        int64_t index = -1;  // an array's element: which, counting through every dimension
    };

    // A value on lines of its own (a struct a member a line), or in one line, shorter (numbers
    // without their hex, pointers named but not followed when they can be).
    enum class Layout { Lines, OneLine };

    // Each fixed-address variable's pieces in memory, by variable index (empty: not one).
    std::unordered_map<size_t, std::vector<MemoryPiece>> memoryPieces;
    // Each byte of memory's variable (index, or -1) and offset into it, for the memory browser.
    std::vector<std::pair<int32_t, uint32_t>> byteOwners;

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
    // An array's value: its numbers in a row when there are few ("{ 1, 2, 3 }"), or else a line
    // an element, the first few; in one line, how many there are.
    std::string FormatArray(uint32_t element, const std::vector<uint32_t>& counts, const std::vector<uint8_t>& bytes,
        const MemoryReader& read, int depth, Layout layout) const;
    // An array's elements a line each ("[2][3] = value"), indented to depth: the first few, or
    // (focus >= 0) those around the focus element, which is in full and marked. Lines say how
    // many more there are before and after.
    std::vector<std::pair<std::string, bool>> ElementLines(uint32_t element, const std::vector<uint32_t>& counts,
        const std::vector<uint8_t>& bytes, const MemoryReader& read, int64_t focus, int depth) const;
    // From a fixed-address variable down to the innermost part of it with the byte at offset:
    // the variable first.
    std::vector<Level> Descend(size_t variable, uint32_t offset) const;
    // What a pointer to type at address points at, by the fixed-address variables:
    // "g_well.Blocks[2][3]". Empty if it's none of theirs, or not the start of something of type.
    std::string NameAt(uint16_t address, uint32_t type) const;
    // The members of a struct that take up its byte at offset (several when bit-fields share it,
    // or in a union).
    std::vector<const Member*> MembersAt(const Type& structType, uint32_t offset) const;
    // A fixed-address variable's bytes at offset (0 where a byte isn't in memory: optimized into
    // a computed value), and which were in memory.
    std::vector<uint8_t> ReadPieces(size_t variable, uint32_t offset, uint32_t size, const MemoryReader& read,
        std::vector<bool>& inMemory) const;
    // Where the bytes at offset are: "$0200-$0233", or that they're spread out, or not in memory.
    std::string PiecesWhere(size_t variable, uint32_t offset, uint32_t size) const;
    Value Evaluate(const Variable& variable, uint16_t pc, const Registers& registers, const MemoryReader& read) const;
    int64_t RegisterValue(const std::string& name, const Registers& registers, const MemoryReader& read) const;
    std::vector<uint8_t> RegisterBytes(const std::string& name, uint32_t size, const Registers& registers, const MemoryReader& read) const;
    std::string Format(uint32_t type, const std::vector<uint8_t>& bytes, const MemoryReader& read, int depth,
        Layout layout = Layout::Lines) const;
};
