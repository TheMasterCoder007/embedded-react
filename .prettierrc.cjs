// JS formatting — intentionally distinct from the C side (.clang-format). Defaults left implicit:
// printWidth 80, tabWidth 2, semi true. `endOfLine: 'auto'` keeps `format:check` green under
// git core.autocrlf=true (CRLF working tree) — it's a portability setting, not a style choice.
module.exports = {
  arrowParens: 'avoid',
  bracketSameLine: true,
  bracketSpacing: false,
  singleQuote: true,
  trailingComma: 'all',
  endOfLine: 'auto',
};
