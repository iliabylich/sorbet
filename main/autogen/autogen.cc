// has to go first because it violates our poisons
#include "msgpack.hpp"

#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "core/Names.h"
#include "main/autogen/autogen.h"

#include "CRC.h"

using namespace std;
namespace sorbet::autogen {

Definition &DefinitionRef::data(ParsedFile &pf) {
    return pf.defs[_id];
}

Reference &ReferenceRef::data(ParsedFile &pf) {
    return pf.refs[_id];
}

class AutogenWalk {
    vector<Definition> defs;
    vector<Reference> refs;
    vector<core::NameRef> requires;
    vector<DefinitionRef> nesting;
    vector<ast::Send *> ignoring;

    UnorderedMap<ast::Expression *, ReferenceRef> ref_map;

    static bool ignoreChild(ast::Expression *expr) {
        bool result = false;

        typecase(expr, [&](ast::Send *send) { result = (send->fun == core::Names::keepForIde()); },

                 [&](ast::EmptyTree *) { result = true; },

                 [&](ast::InsSeq *seq) {
                     result = absl::c_all_of(seq->stats, [](auto &child) { return ignoreChild(child.get()); }) &&
                              ignoreChild(seq->expr.get());
                 },

                 [&](ast::Expression *klass) { result = false; });
        return result;
    }

    static bool definesBehavior(ast::Expression *expr) {
        if (ignoreChild(expr)) {
            return false;
        }
        bool result = true;

        typecase(expr,

                 [&](ast::ClassDef *klass) {
                     auto *id = ast::cast_tree<ast::UnresolvedIdent>(klass->name.get());
                     if (id && id->name == core::Names::singleton()) {
                         // class << self; We consider this
                         // behavior-defining. We could opt to recurse inside
                         // the inner class, but we consider there to be no
                         // valid use of `class << self` solely for namespacing,
                         // so there's no need to support that use case.
                         result = true;
                     } else {
                         result = false;
                     }
                 },

                 [&](ast::Assign *asgn) {
                     if (ast::isa_tree<ast::ConstantLit>(asgn->lhs.get())) {
                         result = false;
                     } else {
                         result = true;
                     }
                 },

                 [&](ast::InsSeq *seq) {
                     result = absl::c_any_of(seq->stats, [](auto &child) { return definesBehavior(child.get()); }) ||
                              definesBehavior(seq->expr.get());
                 },

                 [&](ast::Expression *klass) { result = true; });
        return result;
    }

    vector<core::NameRef> symbolName(core::Context ctx, core::SymbolRef sym) {
        vector<core::NameRef> out;
        while (sym.exists() && sym != core::Symbols::root()) {
            out.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }
        reverse(out.begin(), out.end());
        return out;
    }

    vector<core::NameRef> constantName(core::Context ctx, ast::ConstantLit *cnst) {
        vector<core::NameRef> out;
        while (cnst != nullptr && cnst->original != nullptr) {
            out.emplace_back(cnst->original->cnst);
            cnst = ast::cast_tree<ast::ConstantLit>(cnst->original->scope.get());
        }
        reverse(out.begin(), out.end());
        return out;
    }

public:
    AutogenWalk() {
        auto &def = defs.emplace_back();
        def.id = 0;
        def.type = Definition::Module;
        def.defines_behavior = false;
        def.is_empty = false;
        nesting.emplace_back(def.id);
    }

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        if (!ast::isa_tree<ast::ConstantLit>(original->name.get())) {
            return original;
        }

        // cerr << "preTransformClassDef(" << original->toString(ctx) << ")\n";

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        if (original->kind == ast::Class) {
            def.type = Definition::Class;
        } else {
            def.type = Definition::Module;
        }
        def.is_empty = absl::c_all_of(original->rhs, [](auto &tree) { return ignoreChild(tree.get()); });
        for (auto &ancst : original->ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst && cnst->original != nullptr) {
                def.defines_behavior = true;
            }
        }
        for (auto &ancst : original->singleton_ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst && cnst->original != nullptr) {
                def.defines_behavior = true;
            }
        }
        if (!def.defines_behavior) {
            def.defines_behavior =
                absl::c_any_of(original->rhs, [](auto &tree) { return definesBehavior(tree.get()); });
        }

        // TODO: ref.parent_of, def.parent_ref
        // TODO: expression_range
        original->name = ast::TreeMap::apply(ctx, *this, move(original->name));
        auto it = ref_map.find(original->name.get());
        ENFORCE(it != ref_map.end());
        def.defining_ref = it->second;
        refs[it->second.id()].is_defining_ref = true;
        refs[it->second.id()].definitionLoc = original->loc;

        auto ait = original->ancestors.begin();
        if (original->kind == ast::Class && !original->ancestors.empty()) {
            // Handle the superclass at outer scope
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
            ++ait;
        }
        // Then push a scope
        nesting.emplace_back(def.id);

        for (; ait != original->ancestors.end(); ++ait) {
            *ait = ast::TreeMap::apply(ctx, *this, move(*ait));
        }
        for (auto &ancst : original->singleton_ancestors) {
            ancst = ast::TreeMap::apply(ctx, *this, move(ancst));
        }

        for (auto &ancst : original->ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst.get());
            if (cnst == nullptr || cnst->original == nullptr) {
                // Don't include synthetic ancestors
                continue;
            }

            auto it = ref_map.find(ancst.get());
            if (it == ref_map.end()) {
                continue;
            }
            if (original->kind == ast::Class && &ancst == &original->ancestors.front()) {
                // superclass
                def.parent_ref = it->second;
            }
            refs[it->second.id()].parent_of = def.id;
        }

        return original;
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> original) {
        if (!ast::isa_tree<ast::ConstantLit>(original->name.get())) {
            return original;
        }

        nesting.pop_back();

        return original;
    }

    bool isCBaseConstant(ast::ConstantLit *cnst) {
        while (cnst != nullptr && cnst->original != nullptr) {
            cnst = ast::cast_tree<ast::ConstantLit>(cnst->original->scope.get());
        }
        if (cnst && cnst->typeAliasOrConstantSymbol() == core::Symbols::root()) {
            return true;
        }
        return false;
    }

    unique_ptr<ast::Expression> postTransformConstantLit(core::Context ctx, unique_ptr<ast::ConstantLit> original) {
        if (!ignoring.empty()) {
            return original;
        }
        if (original->original == nullptr) {
            return original;
        }

        auto &ref = refs.emplace_back();
        ref.id = refs.size() - 1;
        if (isCBaseConstant(original.get())) {
            ref.scope = nesting.front();
        } else {
            ref.nesting = nesting;
            reverse(ref.nesting.begin(), ref.nesting.end());
            ref.nesting.pop_back();
            ref.scope = nesting.back();
        }
        ref.loc = original->loc;

        // This will get overridden if this loc is_defining_ref at the point
        // where we set that flag.
        ref.definitionLoc = original->loc;
        ref.name = constantName(ctx, original.get());
        auto sym = original->typeAliasOrConstantSymbol();
        if (!sym.data(ctx)->isClass() || !sym.data(ctx)->derivesFrom(ctx, core::Symbols::StubClass())) {
            ref.resolved = symbolName(ctx, sym);
        }
        ref.is_resolved_statically = true;
        ref.is_defining_ref = false;
        ref_map[original.get()] = ref.id;
        return original;
    }

    unique_ptr<ast::Expression> postTransformAssign(core::Context ctx, unique_ptr<ast::Assign> original) {
        auto *lhs = ast::cast_tree<ast::ConstantLit>(original->lhs.get());
        if (lhs == nullptr || lhs->original == nullptr) {
            return original;
        }

        auto &def = defs.emplace_back();
        def.id = defs.size() - 1;
        auto *rhs = ast::cast_tree<ast::ConstantLit>(original->rhs.get());
        if (rhs && !rhs->typeAlias && rhs->constantSymbol().exists()) {
            def.type = Definition::Alias;
            ENFORCE(ref_map.count(rhs));
            def.aliased_ref = ref_map[rhs];
        } else {
            def.type = Definition::Casgn;
        }
        ENFORCE(ref_map.count(lhs));
        auto &ref = refs[ref_map[lhs].id()];
        def.defining_ref = ref.id;
        ref.is_defining_ref = true;
        ref.definitionLoc = original->loc;

        def.defines_behavior = true;
        def.is_empty = false;

        return original;
    }

    unique_ptr<ast::Send> preTransformSend(core::Context ctx, unique_ptr<ast::Send> original) {
        if (original->fun == core::Names::keepForIde()) {
            ignoring.emplace_back(original.get());
        }
        if ((original->flags & ast::Send::PRIVATE_OK) != 0 && original->fun == core::Names::require() &&
            original->args.size() == 1) {
            auto *lit = ast::cast_tree<ast::Literal>(original->args.front().get());
            if (lit && lit->isString(ctx)) {
                requires.emplace_back(lit->asString(ctx));
            }
        }
        return original;
    }
    unique_ptr<ast::Send> postTransformSend(core::Context ctx, unique_ptr<ast::Send> original) {
        if (!ignoring.empty() && ignoring.back() == original.get()) {
            ignoring.pop_back();
        }
        return original;
    }

    ParsedFile parsedFile() {
        ParsedFile out;
        out.refs = move(refs);
        out.defs = move(defs);
        out.requires = move(requires);
        return out;
    }
};

ParsedFile Autogen::generate(core::Context ctx, unique_ptr<ast::Expression> tree) {
    AutogenWalk walk;
    tree = ast::TreeMap::apply(ctx, walk, move(tree));
    auto pf = walk.parsedFile();
    pf.path = string(tree->loc.file().data(ctx).path());
    auto src = tree->loc.file().data(ctx).source();
    pf.cksum = CRC::Calculate(src.data(), src.size(), CRC::CRC_32());
    pf.tree = move(tree);
    return pf;
}

std::vector<core::NameRef> ParsedFile::fullName(core::Context ctx, DefinitionRef id) {
    auto &def = id.data(*this);
    if (!def.defining_ref.exists()) {
        return {};
    }
    auto &ref = def.defining_ref.data(*this);
    auto scope = fullName(ctx, ref.scope);
    scope.insert(scope.end(), ref.name.begin(), ref.name.end());
    return scope;
}

std::string ParsedFile::toString(core::Context ctx) {
    fmt::memory_buffer out;
    auto nameToString = [&](const auto &nm) -> string { return nm.data(ctx)->show(ctx); };

    fmt::format_to(out,
                   "# ParsedFile: {}\n"
                   "requires: [{}]\n"
                   "## defs:\n",
                   path, fmt::map_join(requires.begin(), requires.end(), ", ", nameToString));

    for (auto &def : defs) {
        string_view type;
        switch (def.type) {
            case Definition::Module:
                type = "module"sv;
                break;
            case Definition::Class:
                type = "class"sv;
                break;
            case Definition::Casgn:
                type = "casgn"sv;
                break;
            case Definition::Alias:
                type = "alias"sv;
                break;
        }

        fmt::format_to(out,
                       "[def id={}]\n"
                       " type={}\n"
                       " defines_behavior={}\n"
                       " is_empty={}\n",
                       def.id.id(), type, (int)def.defines_behavior, (int)def.is_empty);

        if (def.defining_ref.exists()) {
            auto &ref = def.defining_ref.data(*this);
            fmt::format_to(out, " defining_ref=[{}]\n",
                           fmt::map_join(ref.name.begin(), ref.name.end(), " ", nameToString));
        }
        if (def.parent_ref.exists()) {
            auto &ref = def.parent_ref.data(*this);
            fmt::format_to(out, " parent_ref=[{}]\n",
                           fmt::map_join(ref.name.begin(), ref.name.end(), " ", nameToString));
        }
        if (def.aliased_ref.exists()) {
            auto &ref = def.aliased_ref.data(*this);
            fmt::format_to(out, " aliased_ref=[{}]\n",
                           fmt::map_join(ref.name.begin(), ref.name.end(), " ", nameToString));
        }
    }
    fmt::format_to(out, "## refs:\n");
    for (auto &ref : refs) {
        vector<string> nestingStrings;
        for (auto &scope : ref.nesting) {
            auto fullScopeName = fullName(ctx, scope);
            nestingStrings.emplace_back(
                fmt::format("[{}]", fmt::map_join(fullScopeName.begin(), fullScopeName.end(), " ", nameToString)));
        }

        auto refFullName = fullName(ctx, ref.scope);
        fmt::format_to(out,
                       "[ref id={}]\n"
                       " scope=[{}]\n"
                       " name=[{}]\n"
                       " nesting=[{}]\n"
                       " resolved=[{}]\n"
                       " loc={}\n"
                       " is_defining_ref={}\n",

                       ref.id.id(), fmt::map_join(refFullName.begin(), refFullName.end(), " ", nameToString),
                       fmt::map_join(ref.name.begin(), ref.name.end(), " ", nameToString),
                       fmt::join(nestingStrings.begin(), nestingStrings.end(), " "),
                       fmt::map_join(ref.resolved.begin(), ref.resolved.end(), " ", nameToString),
                       ref.loc.filePosToString(ctx), (int)ref.is_defining_ref);

        if (ref.parent_of.exists()) {
            auto parentOfFullName = fullName(ctx, ref.parent_of);
            fmt::format_to(out, " parent_of=[{}]\n",
                           fmt::map_join(parentOfFullName.begin(), parentOfFullName.end(), " ", nameToString));
        }
    }
    return to_string(out);
}

class MsgpackWriter {
private:
    void packName(core::NameRef nm) {
        u4 id;
        auto it = symbolIds.find(nm);
        if (it == symbolIds.end()) {
            id = symbols.size();
            symbols.emplace_back(nm);
            symbolIds[nm] = id;
        } else {
            id = it->second;
        }
        packer.pack_ext(2, 0x00);
        u1 bytes[2] = {(u1)(id >> 8), (u1)(id & 0xff)};
        packer.pack_ext_body(reinterpret_cast<const char *>(&bytes), 2);
    }

    void packNames(vector<core::NameRef> &names) {
        packer.pack_array(names.size());
        for (auto nm : names) {
            packName(nm);
        }
    }

    void packString(string_view str) {
        packer.pack_str(str.size());
        packer.pack_str_body(str.data(), str.size());
    }

    void packString(msgpack::packer<msgpack::sbuffer> &packer, string_view str) {
        packer.pack_str(str.size());
        packer.pack_str_body(str.data(), str.size());
    }

    void packBool(bool b) {
        if (b) {
            packer.pack_true();
        } else {
            packer.pack_false();
        }
    }

    void packReferenceRef(ReferenceRef ref) {
        if (!ref.exists()) {
            packer.pack_nil();
        } else {
            packer.pack_ext(2, 0x02);
            u1 bytes[2] = {(u1)(ref.id() >> 8), (u1)(ref.id() & 0xff)};
            packer.pack_ext_body(reinterpret_cast<const char *>(&bytes), 2);
        }
    }

    void packDefinitionnRef(DefinitionRef ref) {
        if (!ref.exists()) {
            packer.pack_nil();
        } else {
            packer.pack_ext(2, 0x01);
            u1 bytes[2] = {(u1)(ref.id() >> 8), (u1)(ref.id() & 0xff)};
            packer.pack_ext_body(reinterpret_cast<const char *>(&bytes), 2);
        }
    }

    void packRange(u4 begin, u4 end) {
        packer.pack_ext(4, 0x03);
        u1 bytes[4] = {(u1)(begin >> 8), (u1)(begin & 0xff), (u1)(end >> 8), (u1)(end & 0xff)};
        packer.pack_ext_body(reinterpret_cast<const char *>(&bytes), 4);
    }

    void packDefinition(core::Context ctx, ParsedFile &pf, Definition &def) {
        packer.pack_array(def_attrs.size());

        // raw_full_name
        auto raw_full_name = pf.fullName(ctx, def.id);
        packNames(raw_full_name);

        // type
        packer.pack_ext(2, 0x00);
        u1 bytes[2] = {0, (u1)(def.type)};
        packer.pack_ext_body(reinterpret_cast<const char *>(&bytes), 2);

        // defines_behavior
        packBool(def.defines_behavior);

        // is_empty
        packBool(def.is_empty);

        // parent_ref
        packReferenceRef(def.parent_ref);

        // aliased_ref
        packReferenceRef(def.aliased_ref);

        // defining_ref
        packReferenceRef(def.defining_ref);
    }

    void packReference(core::Context ctx, ParsedFile &pf, Reference &ref) {
        packer.pack_array(ref_attrs.size());

        // scope
        packDefinitionnRef(ref.scope.id());

        // name
        packNames(ref.name);

        // nesting
        packer.pack_array(ref.nesting.size());
        for (auto &scope : ref.nesting) {
            packDefinitionnRef(scope.id());
        }

        // expression_range
        auto expression_range = ref.definitionLoc.position(ctx);
        packRange(expression_range.first.line, expression_range.second.line);
        // expression_pos_range
        packRange(ref.loc.beginPos(), ref.loc.endPos());

        // resolved
        if (ref.resolved.empty()) {
            packer.pack_nil();
        } else {
            packNames(ref.resolved);
        }

        // is_resolved_statically
        packer.pack_true();

        // is_defining_ref
        packBool(ref.is_defining_ref);

        // parent_of
        packDefinitionnRef(ref.parent_of);

        // used_ancestors_lhs
        packer.pack_false();
        // used_ancestors
        packer.pack_false();
    }

public:
    // symbols[0..3] are reserved for the Type aliases
    MsgpackWriter() : packer(payload), symbols(4) {}

    std::string pack(core::Context ctx, ParsedFile &pf) {
        packer.pack_array(6);

        packer.pack_true(); // did_resolution
        packString(pf.path);
        packer.pack_uint32(pf.cksum);

        // requires
        packer.pack_array(pf.requires.size());
        for (auto nm : pf.requires) {
            packString(nm.data(ctx)->show(ctx));
        }

        packer.pack_array(pf.defs.size());
        for (auto &def : pf.defs) {
            packDefinition(ctx, pf, def);
        }
        packer.pack_array(pf.refs.size());
        for (auto &ref : pf.refs) {
            packReference(ctx, pf, ref);
        }

        msgpack::sbuffer out;
        msgpack::packer<msgpack::sbuffer> header(out);
        header.pack_map(5);

        packString(header, "symbols");
        int i = -1;
        header.pack_array(symbols.size());
        for (auto sym : symbols) {
            ++i;
            string str;
            switch (i) {
                case Definition::Module:
                    str = "module";
                    break;
                case Definition::Class:
                    str = "class";
                    break;
                case Definition::Casgn:
                    str = "casgn";
                    break;
                case Definition::Alias:
                    str = "alias";
                    break;
                default:
                    str = sym.data(ctx)->show(ctx);
            }
            packString(header, str);
        }

        packString(header, "ref_count");
        header.pack_uint32(pf.refs.size());
        packString(header, "def_count");
        header.pack_uint32(pf.defs.size());

        packString(header, "ref_attrs");
        header.pack_array(ref_attrs.size());
        for (auto attr : ref_attrs) {
            packString(header, attr);
        }

        packString(header, "def_attrs");
        header.pack_array(def_attrs.size());
        for (auto attr : def_attrs) {
            packString(header, attr);
        }
        out.write(payload.data(), payload.size());
        return string(out.data(), out.size());
    }

private:
    msgpack::sbuffer payload;
    msgpack::packer<msgpack::sbuffer> packer;

    vector<core::NameRef> symbols;
    UnorderedMap<core::NameRef, u4> symbolIds;

    static const vector<string> ref_attrs;
    static const vector<string> def_attrs;
};

const vector<string> MsgpackWriter::ref_attrs{
    "scope",
    "name",
    "nesting",
    "expression_range",
    "expression_pos_range",
    "resolved",
    "is_resolved_statically",
    "is_defining_ref",
    "parent_of",
    "used_ancestors_lhs",
    "used_ancestors",
};

const vector<string> MsgpackWriter::def_attrs{
    "raw_full_name", "type", "defines_behavior", "is_empty", "parent_ref", "aliased_ref", "defining_ref",
};

std::string ParsedFile::toMsgpack(core::Context ctx) {
    MsgpackWriter write;
    return write.pack(ctx, *this);
}

} // namespace sorbet::autogen