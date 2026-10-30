// C++: writes and reads a Team (examples/team.bufs).
//   team write FILE        builds the message and saves it
//   team read FILE         prints it
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "bufs/bufs.hpp"
#include "team.bufs.hpp"

using namespace examples;

static const char* roleName(Role role) {
    switch (role) {
    case Role::Guest: return "Guest";
    case Role::Member: return "Member";
    case Role::Admin: return "Admin";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::string mode = argv[1], file = argv[2];
    if (mode == "write") {
        bufs::MessageBuilder mb;
        auto team = mb.initRoot<Team>();
        team.setName("Compilers");
        auto people = team.initPeople(2);

        auto ann = people.init(0);
        ann.setName("Ann");
        ann.setAge(31);
        ann.setRole(Role::Admin);
        ann.setHome(Point{1.5f, 2.5f});
        auto emails = ann.initEmails(2);
        emails.set(0, "ann@example.com");
        emails.set(1, "ann@work.example");
        const double scores[] = {9.5, 7.25};
        ann.setScores(scores, 2);

        auto bob = people.init(1);  // the defaults: age 18, role Member, no emails
        bob.setName("Bob");

        const bufs::Message message = mb.release();
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(message.data()), static_cast<std::streamsize>(message.size()));
        return 0;
    }
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!bufs::verify<Team>(bytes.data(), bytes.size())) {  // always verify bytes you did not write
        std::fprintf(stderr, "not a Team\n");
        return 1;
    }
    const Team::Reader team = bufs::root<Team>(bytes.data());  // reads in place: nothing is copied
    std::printf("team %.*s (%zu people)\n", static_cast<int>(team.name().size()), team.name().data(), team.people().size());
    for (const Person::Reader person : team.people()) {
        std::printf("  %.*s, %u, %s, home (%g, %g)", static_cast<int>(person.name().size()), person.name().data(), person.age(),
                    roleName(person.role()), person.home().x, person.home().y);
        std::printf(", emails [");
        for (size_t i = 0; i < person.emails().size(); ++i)
            std::printf("%s%.*s", i ? ", " : "", static_cast<int>(person.emails()[i].size()), person.emails()[i].data());
        std::printf("], scores [");
        for (size_t i = 0; i < person.scores().size(); ++i)
            std::printf("%s%g", i ? ", " : "", person.scores()[i]);
        std::printf("]\n");
    }
    return 0;
}
