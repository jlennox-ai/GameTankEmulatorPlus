#include "variable_map.h"
#include <cctype>
#include "source_map.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

VariableMap* VariableMap::singleton = nullptr;

// Limits that stop a bad file or a self-referencing pointer from looping or flooding a tooltip.
#define MAX_TYPEDEF_CHAIN 16
#define MAX_FORMAT_DEPTH 3
#define MAX_ARRAY_ELEMENTS_SHOWN 16

namespace {

std::string Field(std::unordered_map<std::string, std::string>& fields, const char* key, const std::string& path) {
    auto found = fields.find(key);
    if(found == fields.end()) {
        throw std::runtime_error(path + ": a line has no " + key);
    }
    return found->second;
}

uint32_t Decimal(std::unordered_map<std::string, std::string>& fields, const char* key, const std::string& path) {
    return (uint32_t) std::stoul(Field(fields, key, path), nullptr, 10);
}

uint32_t Hex(const std::string& text) {
    return (uint32_t) std::stoul(text, nullptr, 16);
}

std::string Unquote(const std::string& text) {
    if((text.size() >= 2) && (text.front() == '"') && (text.back() == '"')) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while(std::getline(stream, part, separator)) {
        if(!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

uint64_t LittleEndian(const std::vector<uint8_t>& bytes) {
    uint64_t value = 0;
    for(size_t i = 0; (i < bytes.size()) && (i < 8); ++i) {
        value |= ((uint64_t) bytes[i]) << (8 * i);
    }
    return value;
}

std::vector<uint8_t> BytesOf(uint64_t value, uint32_t size) {
    std::vector<uint8_t> bytes(size);
    for(uint32_t i = 0; i < size; ++i) {
        bytes[i] = (i < 8) ? (uint8_t) (value >> (8 * i)) : 0;
    }
    return bytes;
}

std::string Hex16(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value & 0xFFFF);
    return text;
}

std::string Indent(int depth) {
    return std::string(2 * depth, ' ');
}

} // namespace

VariableMap::VariableMap(const std::string& path) {
    std::ifstream file(path);
    if(!file.is_open()) {
        throw std::runtime_error("Unable to open " + path);
    }
    std::unordered_map<uint32_t, size_t> variableIndexes;
    std::string line;
    while(std::getline(file, line)) {
        std::string kind;
        std::unordered_map<std::string, std::string> fields;
        if(line.empty() || !parseLine(line, kind, fields)) {
            continue;
        }
        if(kind == "registers") {
            registerBase = (uint16_t) Decimal(fields, "base", path);
        } else if(kind == "scope") {
            Scope scope;
            scope.parent = Decimal(fields, "parent", path);
            for(auto& range : Split(Field(fields, "ranges", path), '+')) {
                std::vector<std::string> ends = Split(range, '-');
                if(ends.size() != 2) {
                    throw std::runtime_error(path + ": bad range " + range);
                }
                scope.ranges.emplace_back(Hex(ends[0]), Hex(ends[1]));
            }
            scopes[Decimal(fields, "id", path)] = scope;
        } else if(kind == "type") {
            Type& type = types[Decimal(fields, "id", path)];
            type.kind = Field(fields, "kind", path);
            type.name = fields.count("name") ? Unquote(fields["name"]) : "";
            type.size = fields.count("size") ? Decimal(fields, "size", path) : 0;
            type.target = fields.count("target") ? Decimal(fields, "target", path) : 0;
            type.encoding = fields.count("encoding") ? fields["encoding"] : "";
            if(fields.count("count")) {
                for(auto& count : Split(fields["count"], 'x')) {
                    type.counts.push_back((uint32_t) std::stoul(count));
                }
            }
        } else if(kind == "member") {
            Member member;
            member.name = Unquote(Field(fields, "name", path));
            member.type = Decimal(fields, "membertype", path);
            member.bitOffset = Decimal(fields, "bitoffset", path);
            member.bitSize = Decimal(fields, "bitsize", path);
            types[Decimal(fields, "type", path)].members.push_back(member);
        } else if(kind == "enumerator") {
            types[Decimal(fields, "type", path)].enumerators.emplace_back(
                std::stoll(Field(fields, "value", path)), Unquote(Field(fields, "name", path)));
        } else if(kind == "var") {
            Variable variable;
            variable.name = Unquote(Field(fields, "name", path));
            variable.type = Decimal(fields, "type", path);
            variable.scope = Decimal(fields, "scope", path);
            variableIndexes[Decimal(fields, "id", path)] = variables.size();
            variables.push_back(variable);
        } else if(kind == "location") {
            auto index = variableIndexes.find(Decimal(fields, "var", path));
            if(index == variableIndexes.end()) {
                throw std::runtime_error(path + ": a location for an unknown variable");
            }
            Location location;
            location.low = Hex(Field(fields, "low", path));
            location.high = Hex(Field(fields, "high", path));
            location.operations = Split(Field(fields, "ops", path), ' ');
            variables[index->second].locations.push_back(location);
        }
    }
}

bool VariableMap::InScope(uint32_t scope, uint16_t pc, int& depth) const {
    depth = 0;
    while(scope != 0) {
        auto found = scopes.find(scope);
        if(found == scopes.end()) {
            return false;
        }
        bool contains = false;
        for(auto& range : found->second.ranges) {
            contains = contains || ((pc >= range.first) && (pc < range.second));
        }
        if(!contains) {
            return false;
        }
        ++depth;
        scope = found->second.parent;
    }
    return true;
}

// The innermost variable of that name in scope at pc (a block's local hides a function's).
const VariableMap::Variable* VariableMap::Find(const std::string& name, uint16_t pc) const {
    const Variable* best = nullptr;
    int bestDepth = -1;
    for(auto& variable : variables) {
        int depth;
        if((variable.name == name) && InScope(variable.scope, pc, depth) && (depth > bestDepth)) {
            best = &variable;
            bestDepth = depth;
        }
    }
    return best;
}

bool VariableMap::IsVisible(const std::string& name, uint16_t pc) const {
    return Find(name, pc) != nullptr;
}

const VariableMap::Type* VariableMap::Resolve(uint32_t type) const {
    for(int i = 0; i < MAX_TYPEDEF_CHAIN; ++i) {
        auto found = types.find(type);
        if(found == types.end()) {
            return nullptr;
        }
        if(found->second.kind != "typedef") {
            return &found->second;
        }
        type = found->second.target;
    }
    return nullptr;
}

uint32_t VariableMap::SizeOf(uint32_t type) const {
    const Type* resolved = Resolve(type);
    if(resolved == nullptr) {
        return 0;
    }
    if(resolved->kind == "array") {
        uint32_t count = 1;
        for(auto dimension : resolved->counts) {
            count *= dimension;
        }
        return count * SizeOf(resolved->target);
    }
    if((resolved->kind == "pointer") || (resolved->kind == "function")) {
        return resolved->size ? resolved->size : 2;
    }
    return resolved->size;
}

std::string VariableMap::TypeName(uint32_t type) const {
    auto found = types.find(type);
    if(found == types.end()) {
        return "void";
    }
    const Type& raw = found->second;
    if(raw.kind == "pointer") {
        return TypeName(raw.target) + "*";
    }
    if(raw.kind == "array") {
        return ArrayTypeName(raw.target, raw.counts);
    }
    if((raw.kind == "struct") || (raw.kind == "union") || (raw.kind == "enum")) {
        return raw.kind + " " + raw.name;
    }
    return raw.name.empty() ? raw.kind : raw.name;
}

int64_t VariableMap::RegisterValue(const std::string& name, const Registers& registers, const MemoryReader& read) const {
    if(name == "A") return registers.a;
    if(name == "X") return registers.x;
    if(name == "Y") return registers.y;
    if((name.size() > 2) && (name.compare(0, 2, "RC") == 0)) {
        return read(registerBase + std::stoi(name.substr(2)));
    }
    if((name.size() > 2) && (name.compare(0, 2, "RS") == 0)) {
        uint16_t low = registerBase + 2 * std::stoi(name.substr(2));
        return read(low) | (read(low + 1) << 8);
    }
    return -1;
}

// A register's bytes, low first: a zero-page register and the ones after it, or A, X or Y.
std::vector<uint8_t> VariableMap::RegisterBytes(const std::string& name, uint32_t size, const Registers& registers, const MemoryReader& read) const {
    std::vector<uint8_t> bytes;
    if((name.size() > 2) && ((name.compare(0, 2, "RC") == 0) || (name.compare(0, 2, "RS") == 0))) {
        uint16_t first = registerBase + std::stoi(name.substr(2)) * ((name[1] == 'S') ? 2 : 1);
        for(uint32_t i = 0; i < size; ++i) {
            bytes.push_back(read(first + i));
        }
        return bytes;
    }
    bytes = BytesOf((uint64_t) RegisterValue(name, registers, read), size);
    return bytes;
}

// The C name of a compiler register: RC26 is __rc26.
static std::string RegisterDisplayName(const std::string& name) {
    if((name.size() > 2) && ((name.compare(0, 2, "RC") == 0) || (name.compare(0, 2, "RS") == 0))) {
        return "__r" + std::string(1, (char) (name[1] - 'A' + 'a')) + name.substr(2);
    }
    return name;
}

// Runs the variable's location expression for pc (its DWARF location, see variables.dbg).
VariableMap::Value VariableMap::Evaluate(const Variable& variable, uint16_t pc, const Registers& registers, const MemoryReader& read) const {
    Value value;
    const Location* location = nullptr;
    for(auto& entry : variable.locations) {
        if((pc >= entry.low) && (pc < entry.high)) {
            location = &entry;
            break;
        }
    }
    if(location == nullptr) {
        value.where = "optimized out here";
        return value;
    }

    enum class Part { None, Register, Value };
    Part part = Part::None;
    std::string partRegister;
    int64_t partValue = 0;
    std::vector<int64_t> stack;
    std::vector<std::string> pieces;
    bool usedPieces = false;
    bool memoryOnly = true;

    // Appends count bytes of the variable from the location described so far.
    auto take = [&](uint32_t count) -> bool {
        if(part == Part::Register) {
            std::vector<uint8_t> bytes = RegisterBytes(partRegister, count, registers, read);
            value.bytes.insert(value.bytes.end(), bytes.begin(), bytes.end());
            pieces.push_back("in " + RegisterDisplayName(partRegister));
            memoryOnly = false;
        } else if(part == Part::Value) {
            std::vector<uint8_t> bytes = BytesOf((uint64_t) partValue, count);
            value.bytes.insert(value.bytes.end(), bytes.begin(), bytes.end());
            pieces.push_back("a computed value");
            memoryOnly = false;
        } else if(!stack.empty()) {
            uint16_t address = (uint16_t) stack.back();
            for(uint32_t i = 0; i < count; ++i) {
                value.bytes.push_back(read(address + i));
            }
            pieces.push_back("at " + Hex16(address));
            if(!usedPieces) {
                value.address = address;
            }
        } else {
            return false;
        }
        part = Part::None;
        stack.clear();
        return true;
    };
    auto unavailable = [&](const std::string& why) {
        Value failed;
        failed.where = why;
        return failed;
    };

    for(auto& operation : location->operations) {
        std::vector<std::string> parts = Split(operation, ':');
        const std::string& name = parts[0];
        bool binary = (name == "plus") || (name == "minus") || (name == "mul") || (name == "div")
            || (name == "mod") || (name == "and") || (name == "or") || (name == "xor") || (name == "shl")
            || (name == "shr");
        if(name == "reg") {
            part = Part::Register;
            partRegister = parts.at(1);
        } else if(name == "breg") {
            int64_t registerValue = RegisterValue(parts.at(1), registers, read);
            if(registerValue < 0) {
                return unavailable("unknown register " + parts.at(1));
            }
            stack.push_back(registerValue + std::stoll(parts.at(2)));
        } else if((name == "addr") || (name == "lit")) {
            stack.push_back(std::stoll(parts.at(1)));
        } else if((name == "plus") && (parts.size() == 2)) {
            if(stack.empty()) return unavailable("bad location expression");
            stack.back() += std::stoll(parts[1]);
        } else if(binary) {
            if(stack.size() < 2) return unavailable("bad location expression");
            int64_t right = stack.back();
            stack.pop_back();
            int64_t& left = stack.back();
            if(((name == "div") || (name == "mod")) && (right == 0)) {
                return unavailable("bad location expression (divides by zero)");
            }
            if(name == "plus") left += right;
            else if(name == "minus") left -= right;
            else if(name == "mul") left *= right;
            else if(name == "div") left /= right;
            else if(name == "mod") left %= right;
            else if(name == "and") left &= right;
            else if(name == "or") left |= right;
            else if(name == "xor") left ^= right;
            else if(name == "shl") left <<= right;
            else left = (int64_t) ((uint64_t) left >> right);
        } else if((name == "neg") || (name == "not")) {
            if(stack.empty()) return unavailable("bad location expression");
            stack.back() = (name == "neg") ? -stack.back() : ~stack.back();
        } else if(name == "deref") {
            if(stack.empty()) return unavailable("bad location expression");
            uint16_t address = (uint16_t) stack.back();
            stack.back() = read(address) | (read(address + 1) << 8);
        } else if(name == "convert") {
            if(stack.empty()) return unavailable("bad location expression");
            uint32_t bits = 8 * std::stoul(parts.at(1));
            if(bits < 64) {
                stack.back() &= (((int64_t) 1) << bits) - 1;
            }
        } else if(name == "stack_value") {
            if(stack.empty()) return unavailable("bad location expression");
            part = Part::Value;
            partValue = stack.back();
        } else if(name == "piece") {
            uint32_t count = std::stoul(parts.at(1));
            usedPieces = true;
            if(!take(count)) {
                // That part of the variable is optimized out.
                value.bytes.insert(value.bytes.end(), count, 0);
                pieces.push_back("part optimized out");
                memoryOnly = false;
            }
        } else if(name == "unavailable") {
            // The build says this location can't be trusted (e.g. a static stack frame).
            std::string why = (parts.size() > 1) ? operation.substr(name.size() + 1) : "not available";
            for(auto& character : why) {
                if(character == '_') character = ' ';
            }
            return unavailable(why);
        } else {
            return unavailable("can't evaluate " + operation);
        }
    }
    if(!usedPieces && !take(SizeOf(variable.type))) {
        return unavailable("bad location expression");
    }
    value.available = true;
    if(usedPieces && memoryOnly) {
        // e.g. a struct split into separate bytes by link-time optimization.
        value.where = "in pieces:";
        for(auto& piece : pieces) {
            value.where += " " + piece.substr(3);
        }
    } else {
        for(size_t i = 0; i < pieces.size(); ++i) {
            value.where += (i ? ", " : "") + pieces[i];
        }
    }
    return value;
}

namespace {

std::vector<uint8_t> MemberBytes(const std::vector<uint8_t>& bytes, uint32_t bitOffset, uint32_t bitSize, uint32_t size) {
    if(bitSize == 0) {
        std::vector<uint8_t> member(size, 0);
        for(uint32_t i = 0; i < size; ++i) {
            if(bitOffset / 8 + i < bytes.size()) {
                member[i] = bytes[bitOffset / 8 + i];
            }
        }
        return member;
    }
    uint64_t field = 0;
    for(uint32_t bit = 0; bit < bitSize; ++bit) {
        uint32_t source = bitOffset + bit;
        if((source / 8 < bytes.size()) && ((bytes[source / 8] >> (source % 8)) & 1)) {
            field |= ((uint64_t) 1) << bit;
        }
    }
    return BytesOf(field, size ? size : 1);
}

} // namespace

std::string VariableMap::ArrayTypeName(uint32_t element, const std::vector<uint32_t>& counts) const {
    std::string name = TypeName(element);
    for(auto dimension : counts) {
        name += "[" + std::to_string(dimension) + "]";
    }
    return name;
}

// An array's elements, when they're few numbers; otherwise how many there are.
std::string VariableMap::FormatArray(uint32_t element, const std::vector<uint32_t>& counts,
    const std::vector<uint8_t>& bytes) const {
    uint32_t count = 1;
    for(auto dimension : counts) {
        count *= dimension;
    }
    uint32_t elementSize = SizeOf(element);
    const Type* resolved = Resolve(element);
    bool scalar = (resolved != nullptr) && ((resolved->kind == "base") || (resolved->kind == "enum"));
    if(!scalar || (elementSize == 0) || (count > MAX_ARRAY_ELEMENTS_SHOWN)) {
        return std::to_string(count) + " elements";
    }
    std::string shown = "{ ";
    for(uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> elementBytes = MemberBytes(bytes, 8 * i * elementSize, 0, elementSize);
        shown += (i ? ", " : "") + std::to_string(LittleEndian(elementBytes));
    }
    return shown + " }";
}

std::string VariableMap::Format(uint32_t type, const std::vector<uint8_t>& bytes, const MemoryReader& read, int depth) const {
    const Type* resolved = Resolve(type);
    if(resolved == nullptr) {
        return "?";
    }
    char text[64];
    uint64_t raw = LittleEndian(bytes);
    uint32_t size = SizeOf(type);
    if(resolved->kind == "base") {
        if(resolved->encoding == "bool") {
            return raw ? "true" : "false";
        }
        int digits = 2 * (int) (size ? size : 1);
        if(resolved->encoding == "signed") {
            int64_t value = (size >= 8) ? (int64_t) raw : (int64_t) (raw << (64 - 8 * size)) >> (64 - 8 * size);
            std::snprintf(text, sizeof(text), "%lld ($%0*llX)", (long long) value, digits, (unsigned long long) raw);
        } else {
            std::snprintf(text, sizeof(text), "%llu ($%0*llX)", (unsigned long long) raw, digits, (unsigned long long) raw);
        }
        return text;
    }
    if(resolved->kind == "enum") {
        for(auto& enumerator : resolved->enumerators) {
            if((uint64_t) enumerator.first == raw) {
                return enumerator.second + " (" + std::to_string(raw) + ")";
            }
        }
        return std::to_string(raw);
    }
    if(resolved->kind == "pointer") {
        std::string shown = Hex16((uint32_t) raw);
        const Type* target = Resolve(resolved->target);
        bool readable = (target != nullptr) && (target->kind != "function") && (target->kind != "array");
        if(readable && (raw != 0) && (depth < MAX_FORMAT_DEPTH)) {
            std::vector<uint8_t> pointed;
            for(uint32_t i = 0; i < SizeOf(resolved->target); ++i) {
                pointed.push_back(read((uint16_t) (raw + i)));
            }
            shown += " -> " + Format(resolved->target, pointed, read, depth + 1);
        }
        return shown;
    }
    if((resolved->kind == "struct") || (resolved->kind == "union")) {
        if(depth >= MAX_FORMAT_DEPTH) {
            return "{...}";
        }
        std::string shown = "{";
        for(auto& member : resolved->members) {
            std::vector<uint8_t> memberBytes = MemberBytes(bytes, member.bitOffset, member.bitSize, SizeOf(member.type));
            shown += "\n" + Indent(depth + 1) + member.name + " = " + Format(member.type, memberBytes, read, depth + 1);
        }
        return shown + "\n" + Indent(depth) + "}";
    }
    if(resolved->kind == "array") {
        return FormatArray(resolved->target, resolved->counts, bytes);
    }
    return Hex16((uint32_t) raw);
}

bool VariableMap::ResolveIndex(const std::string& text, uint16_t pc, const Registers& registers,
    const MemoryReader& read, int64_t& index) const {
    size_t first = text.find_first_not_of(" \t");
    size_t last = text.find_last_not_of(" \t");
    if(first == std::string::npos) {
        return false;
    }
    std::string trimmed = text.substr(first, last - first + 1);
    if(std::isdigit((unsigned char) trimmed[0])) {
        // A number: decimal, or hex with 0x.
        bool hex = (trimmed.size() > 2) && (trimmed[0] == '0') && ((trimmed[1] == 'x') || (trimmed[1] == 'X'));
        std::string digits = hex ? trimmed.substr(2) : trimmed;
        const char* allowed = hex ? "0123456789abcdefABCDEF" : "0123456789";
        if(digits.empty() || (digits.size() > 8) || (digits.find_first_not_of(allowed) != std::string::npos)) {
            return false;
        }
        index = std::stoll(digits, nullptr, hex ? 16 : 10);
        return true;
    }
    // A variable in scope.
    const Variable* variable = Find(trimmed, pc);
    if(variable == nullptr) {
        return false;
    }
    const Type* resolved = Resolve(variable->type);
    if((resolved == nullptr) || ((resolved->kind != "base") && (resolved->kind != "enum"))) {
        return false;
    }
    Value value = Evaluate(*variable, pc, registers, read);
    if(!value.available) {
        return false;
    }
    index = (int64_t) LittleEndian(value.bytes);
    return true;
}

bool VariableMap::Follow(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read,
    ChainValue& result) const {
    if(chain.empty()) {
        return false;
    }
    const Variable* variable = Find(chain[0].second, pc);
    if(variable == nullptr) {
        return false;
    }
    result.expression = chain[0].second;
    for(size_t i = 1; i < chain.size(); ++i) {
        if(chain[i].first == '[') {
            result.expression += "[" + chain[i].second + "]";
        } else {
            result.expression += ((chain[i].first == '>') ? "->" : ".") + chain[i].second;
        }
    }
    Value value = Evaluate(*variable, pc, registers, read);
    if(!value.available) {
        result.error = "  (" + value.where + ")\n" + TypeName(variable->type);
        return true;
    }
    uint32_t type = variable->type;
    // While part of a multi-dimensional array is indexed (g_well[2] of g_well[9][7]): the
    // dimensions left, with type the element type.
    std::vector<uint32_t> arrayCounts;
    std::string where = value.where;
    for(size_t i = 1; i < chain.size(); ++i) {
        const std::string& previous = (chain[i - 1].first == '[') ? "[" + chain[i - 1].second + "]" : chain[i - 1].second;
        if(chain[i].first == '[') {
            int64_t index;
            if(!ResolveIndex(chain[i].second, pc, registers, read, index)) {
                result.error = "\n" + chain[i].second + " isn't a number or a variable in scope";
                return true;
            }
            const Type* resolved = Resolve(type);
            if(arrayCounts.empty() && (resolved != nullptr) && (resolved->kind == "pointer")) {
                uint16_t address = (uint16_t) (LittleEndian(value.bytes) + index * SizeOf(resolved->target));
                type = resolved->target;
                value.bytes.clear();
                for(uint32_t offset = 0; offset < SizeOf(type); ++offset) {
                    value.bytes.push_back(read(address + offset));
                }
                value.address = address;
                where = "at " + Hex16(address);
                continue;
            }
            if(arrayCounts.empty()) {
                if((resolved == nullptr) || (resolved->kind != "array") || resolved->counts.empty()) {
                    result.error = "\n" + previous + " isn't an array or a pointer";
                    return true;
                }
                arrayCounts = resolved->counts;
                type = resolved->target;
            }
            if((index < 0) || (index >= (int64_t) arrayCounts[0])) {
                result.error = "\nindex " + std::to_string(index) + " is outside " + previous + "'s "
                    + std::to_string(arrayCounts[0]) + " elements";
                return true;
            }
            arrayCounts.erase(arrayCounts.begin());
            uint32_t stride = SizeOf(type);
            for(auto count : arrayCounts) {
                stride *= count;
            }
            value.bytes = MemberBytes(value.bytes, 8 * (uint32_t) index * stride, 0, stride);
            if(value.address >= 0) {
                value.address += (int32_t) index * stride;
                where = "at " + Hex16((uint32_t) value.address);
            } else {
                where = "part of " + previous;
            }
            continue;
        }
        if(!arrayCounts.empty()) {
            result.error = "\n" + previous + " is an array: it needs another index";
            return true;
        }
        const Type* resolved = Resolve(type);
        if(chain[i].first == '>') {
            if((resolved == nullptr) || (resolved->kind != "pointer")) {
                result.error = "\n" + previous + " isn't a pointer";
                return true;
            }
            uint16_t address = (uint16_t) LittleEndian(value.bytes);
            type = resolved->target;
            value.bytes.clear();
            for(uint32_t offset = 0; offset < SizeOf(type); ++offset) {
                value.bytes.push_back(read(address + offset));
            }
            value.address = address;
            resolved = Resolve(type);
        }
        if((resolved == nullptr) || ((resolved->kind != "struct") && (resolved->kind != "union"))) {
            result.error = "\n" + previous + " isn't a struct";
            return true;
        }
        const Member* member = nullptr;
        for(auto& candidate : resolved->members) {
            if(candidate.name == chain[i].second) {
                member = &candidate;
            }
        }
        if(member == nullptr) {
            result.error = "\n" + TypeName(type) + " has no " + chain[i].second;
            return true;
        }
        value.bytes = MemberBytes(value.bytes, member->bitOffset, member->bitSize, SizeOf(member->type));
        if((member->bitSize == 0) && (value.address >= 0)) {
            value.address += member->bitOffset / 8;
        } else if(member->bitSize != 0) {
            value.address = -1;
        }
        if(member->bitSize != 0) {
            where = "a bit-field of " + previous;
        } else if(value.address >= 0) {
            where = "at " + Hex16((uint32_t) value.address);
        } else {
            where = "part of " + previous;
        }
        type = member->type;
    }
    result.value = value;
    result.type = type;
    result.arrayCounts = arrayCounts;
    result.where = where;
    return true;
}

std::string VariableMap::Describe(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read) const {
    ChainValue result;
    if(!Follow(chain, pc, registers, read, result)) {
        return "";
    }
    if(!result.error.empty()) {
        return result.expression + result.error;
    }
    if(!result.arrayCounts.empty()) {
        return result.expression + " = " + FormatArray(result.type, result.arrayCounts, result.value.bytes) + "\n"
            + ArrayTypeName(result.type, result.arrayCounts) + ", " + result.where;
    }
    return result.expression + " = " + Format(result.type, result.value.bytes, read, 0) + "\n" + TypeName(result.type)
        + ", " + result.where;
}

bool VariableMap::Locate(const Chain& chain, uint16_t pc, const Registers& registers, const MemoryReader& read,
    uint16_t& address, uint32_t& size) const {
    ChainValue result;
    if(!Follow(chain, pc, registers, read, result) || !result.error.empty() || (result.value.address < 0)) {
        return false;
    }
    address = (uint16_t) result.value.address;
    size = SizeOf(result.type);
    for(auto count : result.arrayCounts) {
        size *= count;
    }
    return true;
}
