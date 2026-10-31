# Upstream HAL patches (`3ds/step5`)

The 3DS build compiles the PC port's HAL from a worktree of
tangosdev/sm64ds-decomp at `origin/release/game-0.9.1`
(`bf42a2418d4396bdd8bb2d61a2cff6cf85c2b743`, "Bump the shipped game exe version
resources to 0.9.1"). The edits it needs there live on the local branch
`3ds/step5`; these files are that branch, exported with `git format-patch`
so this repository holds everything.

Recreate the worktree on a fresh machine:

    git clone https://github.com/tangosdev/sm64ds-decomp.git sm64ds-decomp
    git -C sm64ds-decomp worktree add -b 3ds/step5 ../port-0.9.1 bf42a2418d4396bdd8bb2d61a2cff6cf85c2b743
    git -C port-0.9.1 am ../port3ds/upstream-patches/*.patch

Re-export after committing on `3ds/step5` (from C:\sm64ds-3ds\port-0.9.1):

    git format-patch -N -o ../port3ds/upstream-patches bf42a2418d..3ds/step5

(-N leaves the subjects unnumbered, so an export only adds files: numbered
subjects rewrote every patch's [PATCH n/total] line each time.)
