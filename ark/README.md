# ark/

Сюда кладётся **`libpspsystemctrl_user.a`** — заглушки SystemControl из ARK-4. GH-1.01 вызывает через них прошивку: запуск ISO, PS1, PSN и homebrew.

Где взять: в исходниках [ARK-4](https://github.com/PSP-Archive/ARK-4) она собирается из `libs/SystemCtrlForUser` (Makefile с целью `libpspsystemctrl_user.a`).

Без этого файла проект не соберётся. Сам файл в репозиторий не входит.
