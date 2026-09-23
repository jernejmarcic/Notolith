"""A tiny script embedded whole with ![[script.py]] - source-file embed test."""

from dataclasses import dataclass


@dataclass
class Note:
    title: str
    tags: list[str]

    def slug(self) -> str:
        return self.title.lower().replace(" ", "-")


def main() -> None:
    note = Note("Embedded Source File", ["demo", "embed"])
    print(f"{note.title} -> {note.slug()} {note.tags}")


if __name__ == "__main__":
    main()
