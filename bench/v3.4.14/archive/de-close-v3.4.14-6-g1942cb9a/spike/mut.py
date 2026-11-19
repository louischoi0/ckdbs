import sys

which = sys.argv[1]
if which == "K2":
    p = "src/server/command_dispatcher.cpp"
    s = open(p).read()
    a = """bool CommandDispatcher::MissRefusalAdmitted(const Session& session) const noexcept {
"""
    if s.count(a) != 1:
        sys.exit("K2 anchor")
    s = s.replace(a, a + "    if (true) return false;  // SPIKE K2\n")
    open(p, "w").write(s)
elif which == "K3":
    p = "src/server/expeditor.cpp"
    s = open(p).read()
    a = """    page_reader_.emplace(*device_);
    dispatcher_->set_page_reader(&*page_reader_);
    if (config_.cores == 1) {"""
    if s.count(a) != 1:
        sys.exit("K3 anchor")
    s = s.replace(a, "    if (false) {  // SPIKE K3: no reader, no one-core wake table")
    open(p, "w").write(s)
print("mutated", which)
