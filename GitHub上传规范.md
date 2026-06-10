# ESP32S3_PPP_V1 GitHub 上传规范

## Before Commit

1. Confirm `main.c` was not overloaded with new business logic
2. Update `版本记录.md`
3. Update `记忆文档.md` if there is new hardware/software experience
4. Make sure `build/`, `managed_components/`, `sdkconfig` are not uploaded by accident unless intentionally needed
5. Run at least `idf.py build`

## Commit Habit

- Commit messages should be short and specific
- Recommended style:
  - `feat: add PPP reconnect state handling`
  - `fix: stabilize wifi ap reconfigure flow`
  - `docs: update hardware power notes`

## Push Habit

1. `git status`
2. `git add ...`
3. `git commit -m "type: summary"`
4. `git push origin main`

## Required Documents To Keep Fresh

- `README.md`
- `记忆文档.md`
- `版本记录.md`

## Strong Rules

- Do not upload temporary test garbage
- Do not mix unrelated changes in one commit
- If hardware behavior changed, document it first
