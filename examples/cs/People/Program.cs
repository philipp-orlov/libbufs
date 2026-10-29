// Copyright (c) 2026 Philipp Orlov

// C#: writes and reads a Team (examples/team.bufs).
//   dotnet run -- write FILE     builds the message and saves it
//   dotnet run -- read FILE      prints it
// The C# for the schema is made with bufsc (gen/team.bufs.cs):  bufsc --cs --no-cpp -o gen examples/team.bufs
using Examples;

var (mode, file) = (args[0], args[1]);
if (mode == "write")
{
    var data = Team.Encode(new TeamInit
    {
        Name = "Compilers",
        People = new[]
        {
            new PersonInit
            {
                Name = "Ann", Age = 31, Role = Role.Admin, Home = new Point { X = 1.5f, Y = 2.5f },
                Emails = new[] { "ann@example.com", "ann@work.example" }, Scores = new[] { 9.5, 7.25 },
            },
            new PersonInit { Name = "Bob" },   // the defaults: age 18, role Member, no emails
        },
    });
    File.WriteAllBytes(file, data);
}
else
{
    var team = Team.Decode(File.ReadAllBytes(file));   // reads in place; throws for bytes that are not a Team
    Console.WriteLine($"team {team.Name} ({team.People.Length} people)");
    foreach (var person in team.People)
    {
        Console.WriteLine($"  {person.Name}, {person.Age}, {person.Role}, home ({person.Home.X}, {person.Home.Y}), " +
                          $"emails [{string.Join(", ", person.Emails)}], scores [{string.Join(", ", person.Scores.ToArray())}]");
    }
}
