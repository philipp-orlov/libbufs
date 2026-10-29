# Copyright (c) 2026 Philipp Orlov

"""Python: writes and reads a Team (examples/team.bufs).

    python3 team.py write FILE     builds the message and saves it
    python3 team.py read FILE      prints it

The Python for the schema is made with bufsc (team_bufs.py):  bufsc --py --no-cpp -o gen examples/team.bufs
It needs the `bufs` runtime (libbufs/python) on sys.path; run from a checkout this script adds it.
"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "python"))
sys.path.insert(0, os.path.join(here, "gen"))

from team_bufs import Person, Point, Role, Team  # noqa: E402  (generated)

mode, path = sys.argv[1], sys.argv[2]
if mode == "write":
    data = Team.encode(
        name="Compilers",
        people=[
            dict(name="Ann", age=31, role=Role.Admin, home=Point(x=1.5, y=2.5), emails=["ann@example.com", "ann@work.example"],
                 scores=[9.5, 7.25]),
            dict(name="Bob"),  # the defaults: age 18, role Member, no emails
        ],
    )
    with open(path, "wb") as f:
        f.write(data)
else:
    with open(path, "rb") as f:
        team = Team.decode(f.read())  # reads in place; raises MalformedMessage for bytes that are not a Team
    print(f"team {team.name} ({len(team.people)} people)")
    for person in team.people:
        emails = ", ".join(person.emails)
        scores = ", ".join(f"{s:g}" for s in person.scores)
        print(f"  {person.name}, {person.age}, {Role(person.role).name}, home ({person.home.x:g}, {person.home.y:g}), "
              f"emails [{emails}], scores [{scores}]")
