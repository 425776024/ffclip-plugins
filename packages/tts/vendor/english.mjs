// VideoCut's offline adapter for the MIT HeadTTS rules and Apache-2.0 Misaki lexicons.
// Sources, exact revisions and modifications: ENGLISH-SOURCE.json. No compiled speech engine.
import { Language } from './headtts/language-en-us.mjs';
import us from './us_gold.mjs';
import gb from './gb_gold.mjs';

let engine;
function pronunciation(word, british) {
  const dictionary = british ? gb : us;
  const entry = dictionary[word] ?? dictionary[word.toLowerCase()] ?? dictionary[word.toUpperCase()];
  const value = typeof entry === 'string' ? entry : entry?.DEFAULT;
  if (value) return value;
  // Preserve common inflections rather than treating them as an unrelated OOV word.
  if (/^[A-Za-z]+['’]s$/.test(word)) return pronunciation(word.slice(0, -2), british) + 'z';
  engine ||= new Language();
  let phones = engine.phonemizeWord(engine.normalizeUpper(word).join('')).join('');
  if (british) phones = phones.replace(/O/g, 'Q').replace(/ɜɹ/g, 'ɜː').replace(/([ɑɔə])ɹ(?![AEIOQUYaeiouæɑɐɒɔəɛɪʊʌɜ])/g, '$1ː');
  return phones;
}

/** The legacy adapter contract returns one complete phoneme string in an array. */
export async function phonemize(text, language = 'en-us') {
  if (!['en-us', 'en', 'en-gb'].includes(language)) throw new Error('Unsupported English phonemization language');
  const british = language !== 'en-us';
  const normalized = text.normalize('NFKC').replace(/([a-z])([A-Z])/g, '$1 $2');
  const parts = normalized.match(/[\p{L}]+(?:['’][\p{L}]+)*|\d+(?:\.\d+)?|\s+|[^\p{L}\d\s]/gu) || [];
  const output = parts.map((word) => {
    if (/^\s+$/.test(word)) return ' ';
    if (/^[\p{L}]/u.test(word)) return pronunciation(word, british);
    if (/^\d/.test(word)) {
      engine ||= new Language();
      return engine.generate(word).phonemes.join('');
    }
    return /[;:,.!?—…"“”()]/u.test(word) ? word : ' ';
  }).join('').replace(/\s+/g, ' ').trim();
  if (!output) throw new Error('English text has no pronounceable characters');
  return [output];
}
