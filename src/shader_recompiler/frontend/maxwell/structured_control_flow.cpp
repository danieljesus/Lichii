// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include "common/container/unordered_map.h"
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include <boost/intrusive/list.hpp>

#include <ranges>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"

namespace Shader::Maxwell {
namespace {
struct Statement;

// Use normal_link because we are not guaranteed to destroy the tree in order
using ListBaseHook =
    boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>>;

using Tree = boost::intrusive::list<Statement,
                                    // Allow using Statement without a definition
                                    boost::intrusive::base_hook<ListBaseHook>,
                                    // Avoid linear complexity on splice, size is never called
                                    boost::intrusive::constant_time_size<false>>;
using Node = Tree::iterator;

enum class StatementType {
    Code,
    Goto,
    Label,
    If,
    Loop,
    Break,
    Return,
    Kill,
    Unreachable,
    Function,
    Identity,
    Not,
    Or,
    SetVariable,
    SetIndirectBranchVariable,
    Variable,
    IndirectBranchCond,
};

bool HasChildren(StatementType type) {
    switch (type) {
    case StatementType::If:
    case StatementType::Loop:
    case StatementType::Function:
        return true;
    default:
        return false;
    }
}

struct Goto {};
struct Label {};
struct If {};
struct Loop {};
struct Break {};
struct Return {};
struct Kill {};
struct Unreachable {};
struct FunctionTag {};
struct Identity {};
struct Not {};
struct Or {};
struct SetVariable {};
struct SetIndirectBranchVariable {};
struct Variable {};
struct IndirectBranchCond {};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 26495) // Always initialize a member variable, expected in Statement
#endif
struct Statement : ListBaseHook {
    Statement(const Flow::Block* block_, Statement* up_)
        : block{block_}, up{up_}, type{StatementType::Code} {}
    Statement(Goto, Statement* cond_, Node label_, Statement* up_)
        : label{label_}, cond{cond_}, up{up_}, type{StatementType::Goto} {}
    Statement(Label, u32 id_, Statement* up_) : id{id_}, up{up_}, type{StatementType::Label} {}
    Statement(If, Statement* cond_, Tree&& children_, Statement* up_)
        : children{std::move(children_)}, cond{cond_}, up{up_}, type{StatementType::If} {}
    Statement(Loop, Statement* cond_, Tree&& children_, Statement* up_)
        : children{std::move(children_)}, cond{cond_}, up{up_}, type{StatementType::Loop} {}
    Statement(Break, Statement* cond_, Statement* up_)
        : cond{cond_}, up{up_}, type{StatementType::Break} {}
    Statement(Return, Statement* up_) : up{up_}, type{StatementType::Return} {}
    Statement(Kill, Statement* up_) : up{up_}, type{StatementType::Kill} {}
    Statement(Unreachable, Statement* up_) : up{up_}, type{StatementType::Unreachable} {}
    Statement(FunctionTag) : children{}, type{StatementType::Function} {}
    Statement(Identity, IR::Condition cond_, Statement* up_)
        : guest_cond{cond_}, up{up_}, type{StatementType::Identity} {}
    Statement(Not, Statement* op_, Statement* up_) : op{op_}, up{up_}, type{StatementType::Not} {}
    Statement(Or, Statement* op_a_, Statement* op_b_, Statement* up_)
        : op_a{op_a_}, op_b{op_b_}, up{up_}, type{StatementType::Or} {}
    Statement(SetVariable, u32 id_, Statement* op_, Statement* up_)
        : op{op_}, id{id_}, up{up_}, type{StatementType::SetVariable} {}
    Statement(SetIndirectBranchVariable, IR::Reg branch_reg_, s32 branch_offset_, Statement* up_)
        : branch_offset{branch_offset_},
          branch_reg{branch_reg_}, up{up_}, type{StatementType::SetIndirectBranchVariable} {}
    Statement(Variable, u32 id_, Statement* up_)
        : id{id_}, up{up_}, type{StatementType::Variable} {}
    Statement(IndirectBranchCond, u32 location_, Statement* up_)
        : location{location_}, up{up_}, type{StatementType::IndirectBranchCond} {}

    ~Statement() {
        if (HasChildren(type)) {
            std::destroy_at(&children);
        }
    }

    union {
        const Flow::Block* block;
        Node label;
        Tree children;
        IR::Condition guest_cond;
        Statement* op;
        Statement* op_a;
        u32 location;
        s32 branch_offset;
    };
    union {
        Statement* cond;
        Statement* op_b;
        u32 id;
        IR::Reg branch_reg;
    };
    Statement* up{};
    StatementType type;
    /// Break out of a breakable block: emitted as its own selection, so the code after it is
    /// not nested under it.
    bool selection_break{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

std::string DumpExpr(const Statement* stmt) {
    switch (stmt->type) {
    case StatementType::Identity:
        return fmt::format("{}", stmt->guest_cond);
    case StatementType::Not:
        return fmt::format("!{}", DumpExpr(stmt->op));
    case StatementType::Or:
        return fmt::format("{} || {}", DumpExpr(stmt->op_a), DumpExpr(stmt->op_b));
    case StatementType::Variable:
        return fmt::format("goto_L{}", stmt->id);
    case StatementType::IndirectBranchCond:
        return fmt::format("(indirect_branch == {:x})", stmt->location);
    default:
        return "<invalid type>";
    }
}

[[maybe_unused]] std::string DumpTree(const Tree& tree, u32 indentation = 0) {
    std::string ret;
    std::string indent(indentation, ' ');
    for (auto stmt = tree.begin(); stmt != tree.end(); ++stmt) {
        switch (stmt->type) {
        case StatementType::Code:
            ret += fmt::format("{}    Block {:04x} -> {:04x} ({:#016x});\n", indent,
                               stmt->block->begin.Offset(), stmt->block->end.Offset(),
                               reinterpret_cast<uintptr_t>(stmt->block));
            break;
        case StatementType::Goto:
            ret += fmt::format("{}    if ({}) goto L{};\n", indent, DumpExpr(stmt->cond),
                               stmt->label->id);
            break;
        case StatementType::Label:
            ret += fmt::format("{}L{}:\n", indent, stmt->id);
            break;
        case StatementType::If:
            ret += fmt::format("{}    if ({}) {{\n", indent, DumpExpr(stmt->cond));
            ret += DumpTree(stmt->children, indentation + 4);
            ret += fmt::format("{}    }}\n", indent);
            break;
        case StatementType::Loop:
            ret += fmt::format("{}    do {{\n", indent);
            ret += DumpTree(stmt->children, indentation + 4);
            ret += fmt::format("{}    }} while ({});\n", indent, DumpExpr(stmt->cond));
            break;
        case StatementType::Break:
            ret += fmt::format("{}    if ({}) break;\n", indent, DumpExpr(stmt->cond));
            break;
        case StatementType::Return:
            ret += fmt::format("{}    return;\n", indent);
            break;
        case StatementType::Kill:
            ret += fmt::format("{}    kill;\n", indent);
            break;
        case StatementType::Unreachable:
            ret += fmt::format("{}    unreachable;\n", indent);
            break;
        case StatementType::SetVariable:
            ret += fmt::format("{}    goto_L{} = {};\n", indent, stmt->id, DumpExpr(stmt->op));
            break;
        case StatementType::SetIndirectBranchVariable:
            ret += fmt::format("{}    indirect_branch = {} + {};\n", indent, stmt->branch_reg,
                               stmt->branch_offset);
            break;
        case StatementType::Function:
        case StatementType::Identity:
        case StatementType::Not:
        case StatementType::Or:
        case StatementType::Variable:
        case StatementType::IndirectBranchCond:
            throw LogicError("Statement can't be printed");
        }
    }
    return ret;
}

void SanitizeNoBreaks(const Tree& tree) {
    if (std::ranges::find(tree, StatementType::Break, &Statement::type) != tree.end()) {
        throw NotImplementedException("Capturing statement with break nodes");
    }
}

size_t Level(Node stmt) {
    size_t level{0};
    Statement* node{stmt->up};
    while (node) {
        ++level;
        node = node->up;
    }
    return level;
}

bool IsDirectlyRelated(Node goto_stmt, Node label_stmt) {
    const size_t goto_level{Level(goto_stmt)};
    const size_t label_level{Level(label_stmt)};
    size_t min_level;
    size_t max_level;
    Node min;
    Node max;
    if (label_level < goto_level) {
        min_level = label_level;
        max_level = goto_level;
        min = label_stmt;
        max = goto_stmt;
    } else { // goto_level < label_level
        min_level = goto_level;
        max_level = label_level;
        min = goto_stmt;
        max = label_stmt;
    }
    while (max_level > min_level) {
        --max_level;
        max = max->up;
    }
    return min->up == max->up;
}

bool IsIndirectlyRelated(Node goto_stmt, Node label_stmt) {
    return goto_stmt->up != label_stmt->up && !IsDirectlyRelated(goto_stmt, label_stmt);
}

[[maybe_unused]] bool AreSiblings(Node goto_stmt, Node label_stmt) noexcept {
    Node it{goto_stmt};
    do {
        if (it == label_stmt) {
            return true;
        }
        --it;
    } while (it != goto_stmt->up->children.begin());
    while (it != goto_stmt->up->children.end()) {
        if (it == label_stmt) {
            return true;
        }
        ++it;
    }
    return false;
}

Node SiblingFromNephew(Node uncle, Node nephew) noexcept {
    Statement* const parent{uncle->up};
    Statement* it{&*nephew};
    while (it->up != parent) {
        it = it->up;
    }
    return Tree::s_iterator_to(*it);
}

bool AreOrdered(Node left_sibling, Node right_sibling) noexcept {
    if (left_sibling == right_sibling) {
        return false;
    }
    // Walk forward from both siblings at once: whichever meets the other or the end first
    // decides, so the cost is the shorter walk instead of everything after right_sibling.
    const Node end{right_sibling->up->children.end()};
    for (Node left = left_sibling, right = right_sibling;;) {
        if (++right == left_sibling) {
            return false;
        }
        if (right == end) {
            return true;
        }
        if (++left == right_sibling) {
            return true;
        }
        if (left == end) {
            return false;
        }
    }
}

/// From this many conditional gotos to one label on, the forward ones leave one breakable block
/// instead of each wrapping the rest in a selection: N nested selections are N brackets deep in
/// the shading language (Metal rejects more than 256), and a breakable block is two. Tests change
/// it (SetBreakableBlockGotosForTest) to put the blocks in small programs; 0 never makes one.
std::atomic<u32> breakable_block_gotos{64};

/// A break in `stmt` that leaves the loop enclosing `stmt` (not one inside a loop of its own).
bool HasEnclosingLoopBreak(const Statement& stmt) {
    switch (stmt.type) {
    case StatementType::Break:
        return true;
    case StatementType::If:
        return std::ranges::any_of(stmt.children, HasEnclosingLoopBreak);
    default:
        return false;
    }
}

/// Whether execution can continue past `last`, the last statement of a breakable block's body:
/// a block whose end cannot be reached would have a continue block nothing branches to.
bool FallsThrough(const Statement& last) {
    switch (last.type) {
    case StatementType::Return:
    case StatementType::Unreachable:
        return false;
    case StatementType::Loop:
        // A loop that always repeats ends only through a break of its own
        return !(last.cond->type == StatementType::Identity &&
                 last.cond->guest_cond == IR::Condition{true}) ||
               std::ranges::any_of(last.children, HasEnclosingLoopBreak);
    default:
        return true;
    }
}

bool NeedsLift(Node goto_stmt, Node label_stmt) noexcept {
    const Node sibling{SiblingFromNephew(goto_stmt, label_stmt)};
    return AreOrdered(sibling, goto_stmt);
}

class GotoPass {
public:
    explicit GotoPass(Flow::CFG& cfg, ObjectPool<Statement>& stmt_pool, bool converge_exits_)
        : pool{stmt_pool}, converge_exits{converge_exits_} {
        std::vector gotos{BuildTree(cfg)};
        for (const Node& goto_stmt : gotos) {
            const Statement& cond{*goto_stmt->cond};
            if (cond.type != StatementType::Identity || cond.guest_cond != IR::Condition{true}) {
                ++gotos_per_label[&*goto_stmt->label];
            }
        }
        const auto end{gotos.rend()};
        for (auto goto_stmt = gotos.rbegin(); goto_stmt != end; ++goto_stmt) {
            RemoveGoto(*goto_stmt);
        }
    }

    Statement& RootStatement() noexcept {
        return root_stmt;
    }

private:
    void RemoveGoto(Node goto_stmt) {
        // Force goto_stmt and label_stmt to be directly related
        const Node label_stmt{goto_stmt->label};
        if (IsIndirectlyRelated(goto_stmt, label_stmt)) {
            // Move goto_stmt out using outward-movement transformation until it becomes
            // directly related to label_stmt
            while (!IsDirectlyRelated(goto_stmt, label_stmt)) {
                goto_stmt = MoveOutward(goto_stmt);
            }
        }
        // Force goto_stmt and label_stmt to be siblings
        if (IsDirectlyRelated(goto_stmt, label_stmt)) {
            const size_t label_level{Level(label_stmt)};
            size_t goto_level{Level(goto_stmt)};
            if (goto_level > label_level) {
                // Move goto_stmt out of its level using outward-movement transformations
                while (goto_level > label_level) {
                    goto_stmt = MoveOutward(goto_stmt);
                    --goto_level;
                }
            } else { // Level(goto_stmt) < Level(label_stmt)
                if (NeedsLift(goto_stmt, label_stmt)) {
                    // Lift goto_stmt to above stmt containing label_stmt using goto-lifting
                    // transformations
                    goto_stmt = Lift(goto_stmt);
                }
                // Move goto_stmt into label_stmt's level using inward-movement transformation
                while (goto_level < label_level) {
                    goto_stmt = MoveInward(goto_stmt);
                    ++goto_level;
                }
            }
        }
        // Expensive operation:
        // if (!AreSiblings(goto_stmt, label_stmt)) {
        //     throw LogicError("Goto is not a sibling with the label");
        // }
        // goto_stmt and label_stmt are guaranteed to be siblings, eliminate
        if (std::next(goto_stmt) == label_stmt) {
            // Simply eliminate the goto if the label is next to it
            goto_stmt->up->children.erase(goto_stmt);
        } else if (AreOrdered(goto_stmt, label_stmt)) {
            // Eliminate goto_stmt with a break out of the label's block, or a conditional
            if (!EliminateAsBreak(goto_stmt, label_stmt)) {
                EliminateAsConditional(goto_stmt, label_stmt);
            }
        } else {
            // Eliminate goto_stmt with a loop
            EliminateAsLoop(goto_stmt, label_stmt);
        }
    }

    std::vector<Node> BuildTree(Flow::CFG& cfg) {
        u32 label_id{0};
        std::vector<Node> gotos;
        Flow::Function& first_function{cfg.Functions().front()};
        Node insert_point{root_stmt.children.end()};
        if (converge_exits) {
            // Every EXIT jumps to one return at the end, so all invocations reach the same
            // epilogue (task and mesh programs emit their outputs there, after a barrier).
            Tree& root{root_stmt.children};
            exit_label = root.insert(root.end(), *pool.Create(Label{}, label_id, &root_stmt));
            ++label_id;
            root.insert(root.end(), *pool.Create(Return{}, &root_stmt));
            insert_point = *exit_label;
        }
        BuildTree(cfg, first_function, label_id, gotos, insert_point, std::nullopt);
        return gotos;
    }

    void BuildTree(Flow::CFG& cfg, Flow::Function& function, u32& label_id,
                   std::vector<Node>& gotos, Node function_insert_point,
                   std::optional<Node> return_label) {
        Statement* const false_stmt{pool.Create(Identity{}, IR::Condition{false}, &root_stmt)};
        Tree& root{root_stmt.children};
        ::Common::unordered_map<Flow::Block*, Node> local_labels;
        local_labels.reserve(function.blocks.size());

        for (Flow::Block& block : function.blocks) {
            Statement* const label{pool.Create(Label{}, label_id, &root_stmt)};
            const Node label_it{root.insert(function_insert_point, *label)};
            local_labels.emplace(&block, label_it);
            ++label_id;
        }
        for (Flow::Block& block : function.blocks) {
            const Node label{local_labels.at(&block)};
            // Insertion point
            const Node ip{std::next(label)};

            // Reset goto variables before the first block and after its respective label
            const auto make_reset_variable{[&]() -> Statement& {
                return *pool.Create(SetVariable{}, label->id, false_stmt, &root_stmt);
            }};
            root.push_front(make_reset_variable());
            root.insert(ip, make_reset_variable());
            root.insert(ip, *pool.Create(&block, &root_stmt));

            switch (block.end_class) {
            case Flow::EndClass::Branch: {
                Statement* const always_cond{
                    pool.Create(Identity{}, IR::Condition{true}, &root_stmt)};
                if (block.cond == IR::Condition{true}) {
                    const Node true_label{local_labels.at(block.branch_true)};
                    gotos.push_back(
                        root.insert(ip, *pool.Create(Goto{}, always_cond, true_label, &root_stmt)));
                } else if (block.cond == IR::Condition{false}) {
                    const Node false_label{local_labels.at(block.branch_false)};
                    gotos.push_back(root.insert(
                        ip, *pool.Create(Goto{}, always_cond, false_label, &root_stmt)));
                } else {
                    const Node true_label{local_labels.at(block.branch_true)};
                    const Node false_label{local_labels.at(block.branch_false)};
                    Statement* const true_cond{pool.Create(Identity{}, block.cond, &root_stmt)};
                    gotos.push_back(
                        root.insert(ip, *pool.Create(Goto{}, true_cond, true_label, &root_stmt)));
                    gotos.push_back(root.insert(
                        ip, *pool.Create(Goto{}, always_cond, false_label, &root_stmt)));
                }
                break;
            }
            case Flow::EndClass::IndirectBranch:
                root.insert(ip, *pool.Create(SetIndirectBranchVariable{}, block.branch_reg,
                                             block.branch_offset, &root_stmt));
                for (const Flow::IndirectBranch& indirect : block.indirect_branches) {
                    const Node indirect_label{local_labels.at(indirect.block)};
                    Statement* cond{
                        pool.Create(IndirectBranchCond{}, indirect.address, &root_stmt)};
                    Statement* goto_stmt{pool.Create(Goto{}, cond, indirect_label, &root_stmt)};
                    gotos.push_back(root.insert(ip, *goto_stmt));
                }
                root.insert(ip, *pool.Create(Unreachable{}, &root_stmt));
                break;
            case Flow::EndClass::Call: {
                Flow::Function& call{cfg.Functions()[block.function_call]};
                const Node call_return_label{local_labels.at(block.return_block)};
                BuildTree(cfg, call, label_id, gotos, ip, call_return_label);
                break;
            }
            case Flow::EndClass::Exit:
                if (exit_label) {
                    Statement* const always_cond{
                        pool.Create(Identity{}, IR::Condition{true}, &root_stmt)};
                    gotos.push_back(root.insert(
                        ip, *pool.Create(Goto{}, always_cond, *exit_label, &root_stmt)));
                } else {
                    root.insert(ip, *pool.Create(Return{}, &root_stmt));
                }
                break;
            case Flow::EndClass::Return: {
                Statement* const always_cond{pool.Create(Identity{}, block.cond, &root_stmt)};
                auto goto_stmt{pool.Create(Goto{}, always_cond, return_label.value(), &root_stmt)};
                gotos.push_back(root.insert(ip, *goto_stmt));
                break;
            }
            case Flow::EndClass::Kill:
                root.insert(ip, *pool.Create(Kill{}, &root_stmt));
                break;
            }
        }
    }

    void UpdateTreeUp(Statement* tree) {
        for (Statement& stmt : tree->children) {
            stmt.up = tree;
        }
    }

    void EliminateAsConditional(Node goto_stmt, Node label_stmt) {
        Tree& body{goto_stmt->up->children};
        Tree if_body;
        if_body.splice(if_body.begin(), body, std::next(goto_stmt), label_stmt);
        Statement* const cond{pool.Create(Not{}, goto_stmt->cond, &root_stmt)};
        Statement* const if_stmt{pool.Create(If{}, cond, std::move(if_body), goto_stmt->up)};
        UpdateTreeUp(if_stmt);
        body.insert(goto_stmt, *if_stmt);
        body.erase(goto_stmt);
    }

    /// For a label with many gotos: the code between goto_stmt and the label moves into a
    /// single-pass loop right before the label, and goto_stmt becomes a break at its start.
    /// The next goto to that label (they are removed last to first) extends the same loop, so a
    /// chain of forward gotos becomes `do { if (c1) break; s1; ... if (cN) break; sN }
    /// while (false); label:` instead of N nested selections. Returns false, leaving it to
    /// EliminateAsConditional, when the label has few gotos or when the code to move holds a
    /// break of an enclosing loop, which would then leave the new loop instead.
    bool EliminateAsBreak(Node goto_stmt, Node label_stmt) {
        const u32 threshold{breakable_block_gotos.load(std::memory_order_relaxed)};
        if (threshold == 0 || gotos_per_label[&*label_stmt] < threshold) {
            return false;
        }
        Tree& body{goto_stmt->up->children};
        Statement* block{};
        if (const auto it{breakable_blocks.find(&*label_stmt)};
            it != breakable_blocks.end() && &*std::prev(label_stmt) == it->second) {
            block = it->second;
        }
        const Node moved_end{block ? std::prev(label_stmt) : label_stmt};
        if (std::any_of(std::next(goto_stmt), moved_end, HasEnclosingLoopBreak)) {
            return false;
        }
        // The label is reached only through the gotos when the code before it does not fall
        // through to it (it returns, as a chain of early exits to an error label does).
        if (!block && !FallsThrough(*std::prev(label_stmt))) {
            return false;
        }
        if (!block) {
            Statement* const never{pool.Create(Identity{}, IR::Condition{false}, &root_stmt)};
            block = pool.Create(Loop{}, never, Tree{}, goto_stmt->up);
            body.insert(label_stmt, *block);
            breakable_blocks[&*label_stmt] = block;
        }
        Tree moved;
        moved.splice(moved.begin(), body, std::next(goto_stmt), Tree::s_iterator_to(*block));
        // Only the moved statements change parent: a long chain extends the block once per goto
        for (Statement& stmt : moved) {
            stmt.up = block;
        }
        block->children.splice(block->children.begin(), moved);
        Statement* const exit{pool.Create(Break{}, goto_stmt->cond, block)};
        exit->selection_break = true;
        block->children.push_front(*exit);
        body.erase(goto_stmt);
        return true;
    }

    void EliminateAsLoop(Node goto_stmt, Node label_stmt) {
        Tree& body{goto_stmt->up->children};
        Tree loop_body;
        loop_body.splice(loop_body.begin(), body, label_stmt, goto_stmt);
        Statement* const cond{goto_stmt->cond};
        Statement* const loop{pool.Create(Loop{}, cond, std::move(loop_body), goto_stmt->up)};
        UpdateTreeUp(loop);
        body.insert(goto_stmt, *loop);
        body.erase(goto_stmt);
    }

    [[nodiscard]] Node MoveOutward(Node goto_stmt) {
        switch (goto_stmt->up->type) {
        case StatementType::If:
            return MoveOutwardIf(goto_stmt);
        case StatementType::Loop:
            return MoveOutwardLoop(goto_stmt);
        default:
            throw LogicError("Invalid outward movement");
        }
    }

    [[nodiscard]] Node MoveInward(Node goto_stmt) {
        Statement* const parent{goto_stmt->up};
        Tree& body{parent->children};
        const Node label{goto_stmt->label};
        const Node label_nested_stmt{SiblingFromNephew(goto_stmt, label)};
        const u32 label_id{label->id};

        Statement* const goto_cond{goto_stmt->cond};
        Statement* const set_var{pool.Create(SetVariable{}, label_id, goto_cond, parent)};
        body.insert(goto_stmt, *set_var);

        Tree if_body;
        if_body.splice(if_body.begin(), body, std::next(goto_stmt), label_nested_stmt);
        Statement* const variable{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const neg_var{pool.Create(Not{}, variable, &root_stmt)};
        if (!if_body.empty()) {
            Statement* const if_stmt{pool.Create(If{}, neg_var, std::move(if_body), parent)};
            UpdateTreeUp(if_stmt);
            body.insert(goto_stmt, *if_stmt);
        }
        body.erase(goto_stmt);

        switch (label_nested_stmt->type) {
        case StatementType::If:
            // Update nested if condition
            label_nested_stmt->cond =
                pool.Create(Or{}, variable, label_nested_stmt->cond, &root_stmt);
            break;
        case StatementType::Loop:
            break;
        default:
            throw LogicError("Invalid inward movement");
        }
        Tree& nested_tree{label_nested_stmt->children};
        Statement* const new_goto{pool.Create(Goto{}, variable, label, &*label_nested_stmt)};
        return nested_tree.insert(nested_tree.begin(), *new_goto);
    }

    [[nodiscard]] Node Lift(Node goto_stmt) {
        Statement* const parent{goto_stmt->up};
        Tree& body{parent->children};
        const Node label{goto_stmt->label};
        const u32 label_id{label->id};
        const Node label_nested_stmt{SiblingFromNephew(goto_stmt, label)};

        Tree loop_body;
        loop_body.splice(loop_body.begin(), body, label_nested_stmt, goto_stmt);
        SanitizeNoBreaks(loop_body);
        Statement* const variable{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const loop_stmt{pool.Create(Loop{}, variable, std::move(loop_body), parent)};
        UpdateTreeUp(loop_stmt);
        body.insert(goto_stmt, *loop_stmt);

        Statement* const new_goto{pool.Create(Goto{}, variable, label, loop_stmt)};
        loop_stmt->children.push_front(*new_goto);
        const Node new_goto_node{loop_stmt->children.begin()};

        Statement* const set_var{pool.Create(SetVariable{}, label_id, goto_stmt->cond, loop_stmt)};
        loop_stmt->children.push_back(*set_var);

        body.erase(goto_stmt);
        return new_goto_node;
    }

    Node MoveOutwardIf(Node goto_stmt) {
        const Node parent{Tree::s_iterator_to(*goto_stmt->up)};
        Tree& body{parent->children};
        const u32 label_id{goto_stmt->label->id};
        Statement* const goto_cond{goto_stmt->cond};
        Statement* const set_goto_var{pool.Create(SetVariable{}, label_id, goto_cond, &*parent)};
        body.insert(goto_stmt, *set_goto_var);

        Tree if_body;
        if_body.splice(if_body.begin(), body, std::next(goto_stmt), body.end());
        if_body.pop_front();
        Statement* const cond{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const neg_cond{pool.Create(Not{}, cond, &root_stmt)};
        Statement* const if_stmt{pool.Create(If{}, neg_cond, std::move(if_body), &*parent)};
        UpdateTreeUp(if_stmt);
        body.insert(goto_stmt, *if_stmt);

        body.erase(goto_stmt);

        Statement* const new_cond{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const new_goto{pool.Create(Goto{}, new_cond, goto_stmt->label, parent->up)};
        Tree& parent_tree{parent->up->children};
        return parent_tree.insert(std::next(parent), *new_goto);
    }

    Node MoveOutwardLoop(Node goto_stmt) {
        Statement* const parent{goto_stmt->up};
        Tree& body{parent->children};
        const u32 label_id{goto_stmt->label->id};
        Statement* const goto_cond{goto_stmt->cond};
        Statement* const set_goto_var{pool.Create(SetVariable{}, label_id, goto_cond, parent)};
        Statement* const cond{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const break_stmt{pool.Create(Break{}, cond, parent)};
        body.insert(goto_stmt, *set_goto_var);
        body.insert(goto_stmt, *break_stmt);
        body.erase(goto_stmt);

        const Node loop{Tree::s_iterator_to(*goto_stmt->up)};
        Statement* const new_goto_cond{pool.Create(Variable{}, label_id, &root_stmt)};
        Statement* const new_goto{pool.Create(Goto{}, new_goto_cond, goto_stmt->label, loop->up)};
        Tree& parent_tree{loop->up->children};
        return parent_tree.insert(std::next(loop), *new_goto);
    }

    ObjectPool<Statement>& pool;
    Statement root_stmt{FunctionTag{}};
    bool converge_exits{};
    std::optional<Node> exit_label;
    ::Common::unordered_map<const Statement*, u32> gotos_per_label;
    ::Common::unordered_map<const Statement*, Statement*> breakable_blocks;
};

[[nodiscard]] Statement* TryFindForwardBlock(Statement& stmt) {
    Tree& tree{stmt.up->children};
    const Node end{tree.end()};
    Node forward_node{std::next(Tree::s_iterator_to(stmt))};
    while (forward_node != end && !HasChildren(forward_node->type)) {
        if (forward_node->type == StatementType::Code) {
            return &*forward_node;
        }
        ++forward_node;
    }
    return nullptr;
}

[[nodiscard]] IR::U1 VisitExpr(IR::IREmitter& ir, const Statement& stmt) {
    switch (stmt.type) {
    case StatementType::Identity:
        return ir.Condition(stmt.guest_cond);
    case StatementType::Not:
        return ir.LogicalNot(IR::U1{VisitExpr(ir, *stmt.op)});
    case StatementType::Or:
        return ir.LogicalOr(VisitExpr(ir, *stmt.op_a), VisitExpr(ir, *stmt.op_b));
    case StatementType::Variable:
        return ir.GetGotoVariable(stmt.id);
    case StatementType::IndirectBranchCond:
        return ir.IEqual(ir.GetIndirectBranchVariable(), ir.Imm32(stmt.location));
    default:
        throw NotImplementedException("Statement type {}", stmt.type);
    }
}

class TranslatePass {
public:
    TranslatePass(ObjectPool<IR::Inst>& inst_pool_, ObjectPool<IR::Block>& block_pool_,
                  ObjectPool<Statement>& stmt_pool_, Environment& env_, Statement& root_stmt,
                  IR::AbstractSyntaxList& syntax_list_, const HostTranslateInfo& host_info)
        : stmt_pool{stmt_pool_}, inst_pool{inst_pool_}, block_pool{block_pool_}, env{env_},
          syntax_list{syntax_list_} {
        Visit(root_stmt, nullptr, nullptr);

        IR::Block& first_block{*syntax_list.front().data.block};
        IR::IREmitter ir(first_block, first_block.begin());
        ir.Prologue();
        if (uses_demote_to_helper && host_info.needs_demote_reorder) {
            DemoteCombinationPass();
        }
    }

private:
    void Visit(Statement& parent, IR::Block* break_block, IR::Block* fallthrough_block) {
        IR::Block* current_block{};
        const auto ensure_block{[&] {
            if (current_block) {
                return;
            }
            current_block = block_pool.Create(inst_pool);
            auto& node{syntax_list.emplace_back()};
            node.type = IR::AbstractSyntaxNode::Type::Block;
            node.data.block = current_block;
        }};
        Tree& tree{parent.children};
        for (auto it = tree.begin(); it != tree.end(); ++it) {
            Statement& stmt{*it};
            switch (stmt.type) {
            case StatementType::Label:
                // Labels can be ignored
                break;
            case StatementType::Code: {
                ensure_block();
                Translate(env, current_block, stmt.block->begin.Offset(), stmt.block->end.Offset());
                break;
            }
            case StatementType::SetVariable: {
                ensure_block();
                IR::IREmitter ir{*current_block};
                ir.SetGotoVariable(stmt.id, VisitExpr(ir, *stmt.op));
                break;
            }
            case StatementType::SetIndirectBranchVariable: {
                ensure_block();
                IR::IREmitter ir{*current_block};
                IR::U32 address{ir.IAdd(ir.GetReg(stmt.branch_reg), ir.Imm32(stmt.branch_offset))};
                ir.SetIndirectBranchVariable(address);
                break;
            }
            case StatementType::If: {
                ensure_block();
                IR::Block* const merge_block{MergeBlock(parent, stmt)};

                // Implement if header block
                IR::IREmitter ir{*current_block};
                const IR::U1 cond{ir.ConditionRef(VisitExpr(ir, *stmt.cond))};

                const size_t if_node_index{syntax_list.size()};
                syntax_list.emplace_back();

                // Visit children
                const size_t then_block_index{syntax_list.size()};
                Visit(stmt, break_block, merge_block);

                IR::Block* const then_block{syntax_list.at(then_block_index).data.block};
                current_block->AddBranch(then_block);
                current_block->AddBranch(merge_block);
                current_block = merge_block;

                auto& if_node{syntax_list[if_node_index]};
                if_node.type = IR::AbstractSyntaxNode::Type::If;
                if_node.data.if_node.cond = cond;
                if_node.data.if_node.body = then_block;
                if_node.data.if_node.merge = merge_block;

                auto& endif_node{syntax_list.emplace_back()};
                endif_node.type = IR::AbstractSyntaxNode::Type::EndIf;
                endif_node.data.end_if.merge = merge_block;

                auto& merge{syntax_list.emplace_back()};
                merge.type = IR::AbstractSyntaxNode::Type::Block;
                merge.data.block = merge_block;
                break;
            }
            case StatementType::Loop: {
                IR::Block* const loop_header_block{block_pool.Create(inst_pool)};
                if (current_block) {
                    current_block->AddBranch(loop_header_block);
                }
                auto& header_node{syntax_list.emplace_back()};
                header_node.type = IR::AbstractSyntaxNode::Type::Block;
                header_node.data.block = loop_header_block;

                IR::Block* const continue_block{block_pool.Create(inst_pool)};
                IR::Block* const merge_block{MergeBlock(parent, stmt)};
                // A breakable block (GotoPass::EliminateAsBreak) never repeats
                const bool never_repeats{stmt.cond->type == StatementType::Identity &&
                                         stmt.cond->guest_cond == IR::Condition{false}};
                if (never_repeats) {
                    never_repeating_merges.push_back(merge_block);
                }

                const size_t loop_node_index{syntax_list.size()};
                syntax_list.emplace_back();

                // Visit children
                const size_t body_block_index{syntax_list.size()};
                Visit(stmt, merge_block, continue_block);

                if (never_repeats && continue_block->ImmPredecessors().empty()) {
                    throw LogicError("The body of a breakable block does not reach its end");
                }
                // The continue block is located at the end of the loop
                IR::IREmitter ir{*continue_block};
                const IR::U1 cond{ir.ConditionRef(VisitExpr(ir, *stmt.cond))};

                IR::Block* const body_block{syntax_list.at(body_block_index).data.block};
                loop_header_block->AddBranch(body_block);

                // No back edge in the IR for a loop that never repeats: its header then has one
                // predecessor, so the values read in it are the ones from before it instead of
                // phis with the end of the block, which would hide constants and the handles
                // and pointers the texture and storage buffer passes track. The Repeat node
                // still emits the back edge SPIR-V's structured loops need.
                if (!never_repeats) {
                    continue_block->AddBranch(loop_header_block);
                }
                continue_block->AddBranch(merge_block);

                current_block = merge_block;

                auto& loop{syntax_list[loop_node_index]};
                loop.type = IR::AbstractSyntaxNode::Type::Loop;
                loop.data.loop.body = body_block;
                loop.data.loop.continue_block = continue_block;
                loop.data.loop.merge = merge_block;

                auto& continue_block_node{syntax_list.emplace_back()};
                continue_block_node.type = IR::AbstractSyntaxNode::Type::Block;
                continue_block_node.data.block = continue_block;

                auto& repeat{syntax_list.emplace_back()};
                repeat.type = IR::AbstractSyntaxNode::Type::Repeat;
                repeat.data.repeat.cond = cond;
                repeat.data.repeat.loop_header = loop_header_block;
                repeat.data.repeat.merge = merge_block;
                repeat.data.repeat.never = never_repeats;

                auto& merge{syntax_list.emplace_back()};
                merge.type = IR::AbstractSyntaxNode::Type::Block;
                merge.data.block = merge_block;
                break;
            }
            case StatementType::Break: {
                ensure_block();
                IR::Block* const skip_block{MergeBlock(parent, stmt)};

                IR::IREmitter ir{*current_block};
                const IR::U1 cond{ir.ConditionRef(VisitExpr(ir, *stmt.cond))};
                current_block->AddBranch(break_block);
                current_block->AddBranch(skip_block);
                current_block = skip_block;

                auto& break_node{syntax_list.emplace_back()};
                break_node.type = IR::AbstractSyntaxNode::Type::Break;
                break_node.data.break_node.cond = cond;
                break_node.data.break_node.merge = break_block;
                break_node.data.break_node.skip = skip_block;
                break_node.data.break_node.selection = stmt.selection_break;

                auto& merge{syntax_list.emplace_back()};
                merge.type = IR::AbstractSyntaxNode::Type::Block;
                merge.data.block = skip_block;
                break;
            }
            case StatementType::Return: {
                ensure_block();
                IR::Block* return_block{block_pool.Create(inst_pool)};
                IR::IREmitter{*return_block}.Epilogue();
                current_block->AddBranch(return_block);

                auto& merge{syntax_list.emplace_back()};
                merge.type = IR::AbstractSyntaxNode::Type::Block;
                merge.data.block = return_block;

                current_block = nullptr;
                syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
                break;
            }
            case StatementType::Kill: {
                ensure_block();
                IR::Block* demote_block{MergeBlock(parent, stmt)};
                IR::IREmitter{*current_block}.DemoteToHelperInvocation();
                current_block->AddBranch(demote_block);
                current_block = demote_block;

                auto& merge{syntax_list.emplace_back()};
                merge.type = IR::AbstractSyntaxNode::Type::Block;
                merge.data.block = demote_block;
                uses_demote_to_helper = true;
                break;
            }
            case StatementType::Unreachable: {
                ensure_block();
                current_block = nullptr;
                syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Unreachable;
                break;
            }
            default:
                throw NotImplementedException("Statement type {}", stmt.type);
            }
        }
        if (current_block) {
            if (fallthrough_block) {
                current_block->AddBranch(fallthrough_block);
            } else {
                syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Unreachable;
            }
        }
    }

    IR::Block* MergeBlock(Statement& parent, Statement& stmt) {
        Statement* merge_stmt{TryFindForwardBlock(stmt)};
        if (!merge_stmt) {
            // Create a merge block we can visit later
            merge_stmt = stmt_pool.Create(&dummy_flow_block, &parent);
            parent.children.insert(std::next(Tree::s_iterator_to(stmt)), *merge_stmt);
        }
        return block_pool.Create(inst_pool);
    }

    void DemoteCombinationPass() {
        using Type = IR::AbstractSyntaxNode::Type;
        std::vector<IR::Block*> demote_blocks;
        std::vector<IR::U1> demote_conds;
        u32 num_epilogues{};
        u32 branch_depth{};
        // Loops that never repeat (breakable blocks, GotoPass::EliminateAsBreak): a demote in one
        // is not taken out, as its condition is computed on a path the block may have left.
        u32 never_repeating_depth{};
        const auto never_repeats = [&](const IR::Block* merge) {
            return std::ranges::find(never_repeating_merges, merge) != never_repeating_merges.end();
        };
        for (const IR::AbstractSyntaxNode& node : syntax_list) {
            if (node.type == Type::If) {
                ++branch_depth;
            }
            if (node.type == Type::EndIf) {
                --branch_depth;
            }
            if (node.type == Type::Loop && never_repeats(node.data.loop.merge)) {
                ++never_repeating_depth;
            }
            if (node.type == Type::Repeat && never_repeats(node.data.repeat.merge)) {
                --never_repeating_depth;
            }
            if (node.type != Type::Block) {
                continue;
            }
            for (const IR::Inst& inst : node.data.block->Instructions()) {
                const IR::Opcode op{inst.GetOpcode()};
                if (op == IR::Opcode::DemoteToHelperInvocation) {
                    // Skip reordering nested demote branches.
                    if (branch_depth <= 1 && never_repeating_depth == 0) {
                        demote_blocks.push_back(node.data.block);
                    }
                    break;
                }
                // Returns count at any depth: a nested early return (an EXIT under two
                // conditions, as native fragment programs end their discarded invocations) would
                // otherwise leave before the demote moved to the end, and write its colours.
                if (op == IR::Opcode::Epilogue) {
                    ++num_epilogues;
                }
            }
        }
        if (demote_blocks.size() == 0) {
            return;
        }
        if (num_epilogues > 1) {
            LOG_DEBUG(Shader, "Combining demotes with more than one return is not implemented.");
            return;
        }
        s64 last_iterator_offset{};
        auto& asl{syntax_list};
        for (const IR::Block* demote_block : demote_blocks) {
            const auto start_it{asl.begin() + last_iterator_offset};
            auto asl_it{std::find_if(start_it, asl.end(), [&](const IR::AbstractSyntaxNode& asn) {
                return asn.type == Type::If && asn.data.if_node.body == demote_block;
            })};
            if (asl_it == asl.end()) {
                // Demote without a conditional branch.
                // No need to proceed since all fragment instances will be demoted regardless.
                return;
            }
            const IR::Block* const end_if = asl_it->data.if_node.merge;
            demote_conds.push_back(asl_it->data.if_node.cond);
            last_iterator_offset = std::distance(asl.begin(), asl_it);

            asl_it = asl.erase(asl_it);
            asl_it = std::find_if(asl_it, asl.end(), [&](const IR::AbstractSyntaxNode& asn) {
                return asn.type == Type::Block && asn.data.block == demote_block;
            });

            asl_it = asl.erase(asl_it);
            asl_it = std::find_if(asl_it, asl.end(), [&](const IR::AbstractSyntaxNode& asn) {
                return asn.type == Type::EndIf && asn.data.end_if.merge == end_if;
            });
            asl_it = asl.erase(asl_it);
        }
        const auto epilogue_func{[](const IR::AbstractSyntaxNode& asn) {
            if (asn.type != Type::Block) {
                return false;
            }
            for (const auto& inst : asn.data.block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Epilogue) {
                    return true;
                }
            }
            return false;
        }};
        const auto reverse_it{std::find_if(asl.rbegin(), asl.rend(), epilogue_func)};
        const auto return_block_it{(reverse_it + 1).base()};

        IR::IREmitter ir{*(return_block_it - 1)->data.block};
        IR::U1 cond(IR::Value(false));
        for (const auto& demote_cond : demote_conds) {
            cond = ir.LogicalOr(cond, demote_cond);
        }
        cond.Inst()->DestructiveAddUsage(1);

        IR::AbstractSyntaxNode demote_if_node{};
        demote_if_node.type = Type::If;
        demote_if_node.data.if_node.cond = cond;
        demote_if_node.data.if_node.body = demote_blocks[0];
        demote_if_node.data.if_node.merge = return_block_it->data.block;

        IR::AbstractSyntaxNode demote_node{};
        demote_node.type = Type::Block;
        demote_node.data.block = demote_blocks[0];

        IR::AbstractSyntaxNode demote_endif_node{};
        demote_endif_node.type = Type::EndIf;
        demote_endif_node.data.end_if.merge = return_block_it->data.block;

        const auto next_it_1 = asl.insert(return_block_it, demote_endif_node);
        const auto next_it_2 = asl.insert(next_it_1, demote_node);
        asl.insert(next_it_2, demote_if_node);
    }

    ObjectPool<Statement>& stmt_pool;
    ObjectPool<IR::Inst>& inst_pool;
    ObjectPool<IR::Block>& block_pool;
    Environment& env;
    IR::AbstractSyntaxList& syntax_list;
    bool uses_demote_to_helper{};
    std::vector<IR::Block*> never_repeating_merges;
    const Flow::Block dummy_flow_block{};
};
} // Anonymous namespace

void SetBreakableBlockGotosForTest(u32 gotos) noexcept {
    breakable_block_gotos.store(gotos, std::memory_order_relaxed);
}

IR::AbstractSyntaxList BuildASL(ObjectPool<IR::Inst>& inst_pool, ObjectPool<IR::Block>& block_pool,
                                Environment& env, Flow::CFG& cfg,
                                const HostTranslateInfo& host_info) {
    ObjectPool<Statement> stmt_pool{64};
    const Stage stage{env.ShaderStage()};
    GotoPass goto_pass{cfg, stmt_pool, stage == Stage::Task || stage == Stage::Mesh};
    Statement& root{goto_pass.RootStatement()};
    IR::AbstractSyntaxList syntax_list;
    TranslatePass{inst_pool, block_pool, stmt_pool, env, root, syntax_list, host_info};
    stmt_pool.ReleaseContents();
    return syntax_list;
}

} // namespace Shader::Maxwell
