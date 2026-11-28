# EM-S0 spike applier: `python3 apply_spike.py <worktree>`; copies em_spike.hpp
# into include/kds/base/ and inserts one Scope per site. Never committed.
import sys, os, shutil
root = sys.argv[1]
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'em_spike.hpp'),
            os.path.join(root, 'include/kds/base/em_spike.hpp'))
INC = '#include "kds/base/em_spike.hpp"\n'


def ins(path, anchor, add):
    p = os.path.join(root, path)
    s = open(p).read()
    assert anchor in s, (path, anchor)
    s = s.replace(anchor, anchor + add, 1)
    if INC not in s:
        i = s.index('#include')
        s = s[:i] + INC + s[i:]
    open(p, 'w').write(s)


def scope(cat):
    return '    kds::em_spike::Scope em_scope_(kds::em_spike::%s);\n' % cat


P = 'src/parser/parser.cpp'
ins(P, 'StatusOr<Statement> Parser::Parse() {\n', scope('kParse'))
ins(P, 'StatusOr<Query> Parser::ParseQuery() {\n', scope('kParse'))
ins(P, 'StatusOr<std::vector<Condition>> Parser::ParseConditions() {\n', scope('kParse'))
ins('src/parser/fingerprint.cpp', 'std::optional<Fingerprint> FingerprintOf(std::string_view sql) {\n', scope('kFingerprint'))
ins('src/parser/lexer.cpp', 'std::vector<StatementArgument> ArgumentsOf(std::string_view sql) {\n', scope('kArguments'))
K = 'src/server/kwp_session.cpp'
ins(K, '                                 const std::vector<wire::BoundParam>& params) {\n', scope('kSubstitute'))
ins(K, 'std::uint32_t PlaceholderCount(std::string_view sql) {\n', scope('kCount'))
ins('src/exec/step_compiler.cpp',
    '                            const StatementContext* context, const PlanOptions* plan) {\n',
    scope('kCompile'))
ins('src/catalog/catalog.cpp',
    'StatusOr<Oid> Catalog::FindNamespaceOidByName(std::string_view name,\n'
    '                                              const txn::ReadView* view) {\n',
    scope('kNamespace'))
KP = 'src/parser/kept_parse.cpp'
ins(KP, 'std::optional<KeptParse> KeptParse::Of(std::string_view sql) {\n', scope('kKeptOf'))
ins(KP, 'bool HolesDelimited(std::string_view sql) {\n', scope('kHoles'))
ins(KP, '                                                     std::string text) {\n', scope('kBind'))
ins(KP, 'Statement BoundStatement::Materialize() const {\n', scope('kMaterialize'))
D = os.path.join(root, 'src/server/command_dispatcher.cpp')
s = open(D).read()
old = '        return {"PONG", false};'
assert old in s
s = s.replace(old, '        return {"PONG" + kds::em_spike::Report(), false};', 1)
i = s.index('#include')
s = s[:i] + INC + s[i:]
open(D, 'w').write(s)
print("ok")
