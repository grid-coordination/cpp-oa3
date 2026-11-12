#include <oa3/model/entities.h>

#include <string.h>

namespace oa3 {

namespace {

struct ObjectTypeName {
    ObjectType type;
    const char* name;
};

const ObjectTypeName kObjectTypes[] = {
    {ObjectType::kProgram, "PROGRAM"}, {ObjectType::kEvent, "EVENT"},
    {ObjectType::kReport, "REPORT"},   {ObjectType::kSubscription, "SUBSCRIPTION"},
    {ObjectType::kVen, "VEN"},         {ObjectType::kResource, "RESOURCE"},
};

struct OperationName {
    Operation op;
    const char* name;
};

// The first name of each operation is the one to_string() returns.
const OperationName kOperations[] = {
    {Operation::kCreate, "CREATE"}, {Operation::kRead, "READ"},   {Operation::kUpdate, "UPDATE"},
    {Operation::kDelete, "DELETE"}, {Operation::kCreate, "POST"}, {Operation::kRead, "GET"},
    {Operation::kUpdate, "PUT"},
};

}  // namespace

ObjectType parse_object_type(const char* s) {
    if (s == nullptr) {
        return ObjectType::kUnknown;
    }
    for (const ObjectTypeName& t : kObjectTypes) {
        if (strcmp(s, t.name) == 0) {
            return t.type;
        }
    }
    return ObjectType::kUnknown;
}

Operation parse_operation(const char* s) {
    if (s == nullptr) {
        return Operation::kUnknown;
    }
    for (const OperationName& o : kOperations) {
        if (strcmp(s, o.name) == 0) {
            return o.op;
        }
    }
    return Operation::kUnknown;
}

const char* to_string(ObjectType t) {
    for (const ObjectTypeName& n : kObjectTypes) {
        if (n.type == t) {
            return n.name;
        }
    }
    return "UNKNOWN";
}

const char* to_string(Operation op) {
    for (const OperationName& n : kOperations) {
        if (n.op == op) {
            return n.name;
        }
    }
    return "UNKNOWN";
}

const Payload* find_payload(const Interval& interval, const char* type) {
    if (type == nullptr) {
        return nullptr;
    }
    for (uint8_t i = 0; i < interval.payload_count && i < OA3_MAX_PAYLOADS; ++i) {
        if (strcmp(interval.payloads[i].type, type) == 0) {
            return &interval.payloads[i];
        }
    }
    return nullptr;
}

const Value* payload_value(const Event& event, const Payload& payload, size_t i) {
    if (i >= payload.value_count) {
        return nullptr;
    }
    size_t index = static_cast<size_t>(payload.first_value) + i;
    if (index >= event.value_count || index >= OA3_MAX_VALUES) {
        return nullptr;
    }
    return &event.values[index];
}

namespace {

const PayloadDescriptor* find_in(const PayloadDescriptor* descriptors, uint8_t count,
                                 const char* payload_type) {
    if (payload_type == nullptr) {
        return nullptr;
    }
    for (uint8_t i = 0; i < count && i < OA3_MAX_DESCRIPTORS; ++i) {
        if (strcmp(descriptors[i].payload_type, payload_type) == 0) {
            return &descriptors[i];
        }
    }
    return nullptr;
}

}  // namespace

const PayloadDescriptor* find_descriptor(const Event& event, const char* payload_type) {
    return find_in(event.descriptors, event.descriptor_count, payload_type);
}

const PayloadDescriptor* find_descriptor(const Program& program, const char* payload_type) {
    return find_in(program.descriptors, program.descriptor_count, payload_type);
}

}  // namespace oa3
