// Copyright (c) 2026 Philipp Orlov

// TypeScript: writes and reads a Team (examples/team.bufs).
//   node team.mjs write FILE     builds the message and saves it
//   node team.mjs read FILE      prints it
// The TypeScript for the schema is made with bufsc (gen/team.bufs.ts):  bufsc --ts --no-cpp -o gen examples/team.bufs
// and the runtime it imports ('@bufs/runtime') is libbufs/ts/src/bufs.ts.
import * as fs from 'node:fs';
import { Role, Team } from './gen/team.bufs';

const [mode, file] = process.argv.slice(2);
if (mode === 'write') {
  const data = Team.encode({
    name: 'Compilers',
    people: [
      { name: 'Ann', age: 31, role: Role.Admin, home: { x: 1.5, y: 2.5 }, emails: ['ann@example.com', 'ann@work.example'], scores: [9.5, 7.25] },
      { name: 'Bob' }, // the defaults: age 18, role Member, no emails
    ],
  });
  fs.writeFileSync(file, data);
} else {
  const team = Team.decode(fs.readFileSync(file)); // reads in place; throws MalformedMessage for bytes that are not a Team
  console.log(`team ${team.name} (${team.people.length} people)`);
  for (const person of team.people) {
    console.log(
      `  ${person.name}, ${person.age}, ${Role[person.role]}, home (${person.home.x}, ${person.home.y}), ` +
        `emails [${person.emails.join(', ')}], scores [${person.scores.join(', ')}]`,
    );
  }
}
