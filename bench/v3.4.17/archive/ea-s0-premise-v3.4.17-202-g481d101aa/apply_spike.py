import sys, os
root = sys.argv[1]
INC = '#include "kds/base/ea_spike.hpp"\n'

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
    return '    kds::ea_spike::Scope ea_scope_(kds::ea_spike::%s);\n' % cat

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
D = os.path.join(root, 'src/server/command_dispatcher.cpp')
s = open(D).read()
old = '        return {"PONG", false};'
assert old in s
s = s.replace(old, '        return {"PONG" + kds::ea_spike::Report(), false};', 1)
i = s.index('#include')
s = s[:i] + INC + s[i:]
open(D, 'w').write(s)
print("ok")
