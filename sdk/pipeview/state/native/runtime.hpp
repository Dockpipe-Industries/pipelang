#ifndef PIPELANG_PIPEVIEW_STATE_RUNTIME_V1_HPP
#define PIPELANG_PIPEVIEW_STATE_RUNTIME_V1_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pipelang::pipeview::state {

enum class Operation { Set, Save, Revert };
enum class Outcome { Success, Cancelled, Failure };
enum class ValueKind { Boolean, Integer, Text, Invocation };

using Value = std::variant<bool, std::int64_t, std::string>;
using Values = std::map<std::string, Value>;
using Validator = std::function<bool(const Values&)>;

struct Action {
    std::string name;
    ValueKind payloadKind;
    Operation operation;
    std::string target;
};

struct Definition {
    Values initial;
    std::vector<Action> actions;
};

struct SaveRequest {
    std::uint64_t id;
    Values snapshot;
};

struct Snapshot {
    Values draft;
    Values saved;
    bool dirty;
    bool valid;
    bool saving;
    bool canSave;
    std::string status;
};

// Synchronous, single-owner state. Hosts serialize calls and own persistence.
// Validators must be deterministic and must not retain their input reference.
class Model {
public:
    static std::unique_ptr<Model> create(
        Definition definition,
        std::optional<Values> saved,
        Validator validator,
        std::string& error)
    {
        error.clear();
        if (definition.initial.empty() || definition.initial.size() > 128) {
            error = "state count must be between 1 and 128";
            return nullptr;
        }
        for (const auto& [name, value] : definition.initial) {
            if (!validIdentity(name) || !validScalar(value)) {
                error = "invalid state " + name;
                return nullptr;
            }
        }
        if (definition.actions.empty() || definition.actions.size() > 128) {
            error = "action count must be between 1 and 128";
            return nullptr;
        }

        std::map<std::string, Action> actions;
        unsigned int saveCount = 0;
        unsigned int revertCount = 0;
        for (const auto& action : definition.actions) {
            if (!validIdentity(action.name) || actions.count(action.name) != 0) {
                error = "invalid or duplicate action name " + action.name;
                return nullptr;
            }
            switch (action.operation) {
            case Operation::Set: {
                const auto target = definition.initial.find(action.target);
                if (target == definition.initial.end() || kind(target->second) != action.payloadKind) {
                    error = "set action requires a matching state target and scalar payload";
                    return nullptr;
                }
                break;
            }
            case Operation::Save:
            case Operation::Revert:
                if (action.payloadKind != ValueKind::Invocation || !action.target.empty()) {
                    error = "save and revert require invocation payload and no target";
                    return nullptr;
                }
                if (action.operation == Operation::Save)
                    ++saveCount;
                else
                    ++revertCount;
                break;
            default:
                error = "unsupported operation";
                return nullptr;
            }
            actions.emplace(action.name, action);
        }
        if (saveCount > 1 || revertCount > 1) {
            error = "at most one save action and one revert action are allowed";
            return nullptr;
        }

        Values baseline = saved ? *saved : definition.initial;
        if (!sameShape(definition.initial, baseline)) {
            error = "saved state must contain exactly the declared canonical scalar types";
            return nullptr;
        }
        if (!validator)
            validator = [](const Values&) { return true; };
        if (!validate(validator, definition.initial)) {
            error = "initial state failed validation";
            return nullptr;
        }
        if (!validate(validator, baseline)) {
            error = "saved state failed validation";
            return nullptr;
        }
        return std::unique_ptr<Model>(new Model(std::move(actions), std::move(baseline), std::move(validator)));
    }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // On success, request is populated only when dispatch starts a save.
    // Outputs are cleared on entry; a refusal never changes model state.
    bool dispatch(
        const std::string& name,
        std::optional<Value> payload,
        std::optional<SaveRequest>& request,
        std::string& error)
    {
        request.reset();
        error.clear();
        if (pending_) {
            error = "a save is already pending";
            return false;
        }
        const auto found = actions_.find(name);
        if (found == actions_.end()) {
            error = "unknown action " + name;
            return false;
        }
        const Action& action = found->second;
        if (action.operation == Operation::Set) {
            if (!payload || !validScalar(*payload) || kind(*payload) != action.payloadKind) {
                error = "set action requires a canonical matching payload";
                return false;
            }
            Values draft = draft_;
            draft.at(action.target) = *payload;
            const bool valid = validate(validator_, draft);
            draft_ = std::move(draft);
            valid_ = valid;
            refreshStatus();
            return true;
        }
        if (payload) {
            error = "action does not accept a payload";
            return false;
        }
        if (action.operation == Operation::Revert) {
            draft_ = saved_;
            valid_ = true;
            status_ = "Saved";
            return true;
        }
        if (!valid_ || draft_ == saved_) {
            error = "save requires valid unsaved changes";
            return false;
        }
        if (!validate(validator_, draft_)) {
            error = "save candidate failed validation";
            return false;
        }
        if (nextID_ == std::numeric_limits<std::uint64_t>::max()) {
            error = "save request identity capacity exhausted";
            return false;
        }
        const SaveRequest captured{nextID_ + 1, draft_};
        pending_ = captured;
        nextID_ = captured.id;
        status_ = "Saving";
        request = captured;
        return true;
    }

    bool complete(std::uint64_t id, Outcome outcome, std::string& error)
    {
        error.clear();
        if (!pending_ || id == 0 || pending_->id != id) {
            error = "save request is not pending";
            return false;
        }
        switch (outcome) {
        case Outcome::Success:
            saved_ = pending_->snapshot;
            status_ = "Saved";
            break;
        case Outcome::Failure:
            status_ = "Save failed";
            break;
        case Outcome::Cancelled:
            status_ = "Save cancelled";
            break;
        default:
            error = "unsupported save outcome";
            return false;
        }
        pending_.reset();
        return true;
    }

    Snapshot snapshot() const
    {
        const bool dirty = draft_ != saved_;
        const bool saving = pending_.has_value();
        return {draft_, saved_, dirty, valid_, saving, dirty && valid_ && !saving, status_};
    }

private:
    Model(std::map<std::string, Action> actions, Values baseline, Validator validator)
        : actions_(std::move(actions)),
          draft_(baseline),
          saved_(std::move(baseline)),
          validator_(std::move(validator))
    {
    }

    static bool validIdentity(const std::string& identity)
    {
        if (identity.empty() || identity.size() > 128)
            return false;
        for (const unsigned char character : identity) {
            if ((character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '.' || character == '-' || character == '_')
                continue;
            return false;
        }
        return true;
    }

    static bool validText(const std::string& text)
    {
        if (text.size() > 4096)
            return false;
        std::size_t position = 0;
        while (position < text.size()) {
            const unsigned char leading = static_cast<unsigned char>(text[position++]);
            if (leading <= 0x7f)
                continue;

            unsigned int continuationCount;
            std::uint32_t scalar;
            std::uint32_t minimum;
            if (leading >= 0xc2 && leading <= 0xdf) {
                continuationCount = 1;
                scalar = leading & 0x1f;
                minimum = 0x80;
            } else if (leading >= 0xe0 && leading <= 0xef) {
                continuationCount = 2;
                scalar = leading & 0x0f;
                minimum = 0x800;
            } else if (leading >= 0xf0 && leading <= 0xf4) {
                continuationCount = 3;
                scalar = leading & 0x07;
                minimum = 0x10000;
            } else {
                return false;
            }
            if (continuationCount > text.size() - position)
                return false;
            for (unsigned int index = 0; index < continuationCount; ++index) {
                const unsigned char continuation = static_cast<unsigned char>(text[position++]);
                if ((continuation & 0xc0) != 0x80)
                    return false;
                scalar = (scalar << 6) | (continuation & 0x3f);
            }
            if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
                return false;
        }
        return true;
    }

    static bool validScalar(const Value& value)
    {
        if (value.valueless_by_exception())
            return false;
        const auto text = std::get_if<std::string>(&value);
        return text == nullptr || validText(*text);
    }

    static ValueKind kind(const Value& value)
    {
        if (std::holds_alternative<bool>(value))
            return ValueKind::Boolean;
        if (std::holds_alternative<std::int64_t>(value))
            return ValueKind::Integer;
        return ValueKind::Text;
    }

    static bool sameShape(const Values& declared, const Values& values)
    {
        if (declared.size() != values.size())
            return false;
        for (const auto& [name, expected] : declared) {
            const auto value = values.find(name);
            if (value == values.end() || !validScalar(value->second) || kind(value->second) != kind(expected))
                return false;
        }
        return true;
    }

    static bool validate(const Validator& validator, const Values& values)
    {
        const Values owned = values;
        return validator(owned);
    }

    void refreshStatus()
    {
        if (!valid_) {
            status_ = "Invalid settings";
        } else if (draft_ != saved_) {
            status_ = "Unsaved changes";
        } else {
            status_ = "Saved";
        }
    }

    std::map<std::string, Action> actions_;
    Values draft_;
    Values saved_;
    Validator validator_;
    bool valid_ = true;
    std::string status_ = "Saved";
    std::uint64_t nextID_ = 0;
    std::optional<SaveRequest> pending_;
};

} // namespace pipelang::pipeview::state

#endif // PIPELANG_PIPEVIEW_STATE_RUNTIME_V1_HPP
