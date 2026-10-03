#!/bin/zsh -l
cd -- "${0:A:h}" || exit 1
npm run release:npm -- "$@"
result=$?
if [[ -t 0 ]]; then
  read '?按回车关闭窗口…'
fi
exit "$result"
