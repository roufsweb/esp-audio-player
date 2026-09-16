---
name: github-workflow
description: Helper skill for managing open-source GitHub workflows (commits, PRs, issue sync) for the ESP Audio Player.
---

# GitHub Workflow Skill

Use this skill when managing git version control, commits, and pull requests for the ESP Audio Player project.

## Workflow Principles
1. Verify repository status: Always run `git status` and `git diff` before staging.
2. Verify build integrity: Ensure the code builds cleanly (`idf.py build`) prior to committing any functional change.
3. Keep living documentation synchronized: If a commit introduces new pins, changes memory configuration, or adds console commands, update `HARDWARE.md` and `README.md` in the same commit.
4. Conventional Commits (No emojis):
   - `feat: <description>` for new capabilities
   - `fix: <description>` for bug fixes and stack patches
   - `docs: <description>` for documentation updates
   - `refactor: <description>` for non-functional code restructuring
   - `chore: <description>` for build scripts, sdkconfig, or tooling updates
5. Atomic commits: Separate hardware pinout reassignments from application-level logic.
