// Adapted from @met4citizen/headtts 1.3.0; Copyright 2025 Mika Suominen, MIT.
// VideoCut 2026-10-01: remove dictionary I/O and unused runtime utilities; dictionaries are bundled offline.
import * as utils from "./utils.mjs";

// Base class for language modules
class LanguageBase {

  /**
  * @constructor
  */
  constructor( settings = null ) {
    this.settings = Object.assign({
      trace: false
    }, settings || {});

    // Whitespace characters
    this.whitespaces = {
      " ": " ", "\n": "\n", "\t": "\t", "\r": "\r", "\v": "\v", "\f": "\f",
      "\u00A0": "\u00A0", "\u1680": "\u1680", "\u2000": "\u2000",
      "\u2001": "\u2001", "\u2002": "\u2002", "\u2003": "\u2003",
      "\u2004": "\u2004", "\u2005": "\u2005", "\u2006": "\u2006",
      "\u2007": "\u2007", "\u2008": "\u2008",
      "\u2009": "\u2009", "\u200A": "\u200A", "\u2028": "\u2028",
      "\u2029": "\u2029", "\u202F": "\u202F", "\u205F": "\u205F"
    };

    // Misaki tokens to Oculus visemes
    this.misakiToOculusViseme = {
      "$": null, ";": null, ":": null, ",": null, ".": null, "!": null,
      "?": null, "—": null, "…": null, "\"": null, "(": null, ")": null,
      "“": null, "”": null, " ": null, "\u0303": null, "ʣ": "DD", "ʥ": "CH",
      "ʦ": "CH", "ʨ": "CH", "ᵝ": null, "ꭧ": null, "A": "E", "I": "I",
      "O": "O", "Q": "O", "S": "SS", "T": "DD", "W": "U", "Y": "I",
      "ᵊ": null, "a": "aa", "b": "PP", "c": "kk", "d": "DD", "e": "E",
      "f": "FF", "h": null, "i": "I", "j": "I", "k": "kk", "l": "RR",
      "m": "PP", "n": "nn", "o": "O", "p": "PP", "q": "kk", "r": "RR",
      "s": "SS", "t": "DD", "u": "U", "v": "FF", "w": "U", "x": "SS",
      "y": "I", "z": "SS", "ɑ": "aa", "ɐ": "aa", "ɒ": "aa", "æ": "aa",
      "β": "FF", "ɔ": "O", "ɕ": "SS", "ç": "SS", "ɖ": "DD", "ð": "TH", "ʤ":
      "CH", "ə": "E", "ɚ": "RR", "ɛ": "E", "ɜ": "E", "ɟ": "DD", "ɡ": "kk",
      "ɥ": "U", "ɨ": "I", "ɪ": "I", "ʝ": "I", "ɯ": "U", "ɰ": "U", "ŋ": "nn",
      "ɳ": "nn", "ɲ": "nn", "ɴ": "nn", "ø": "O", "ɸ": "FF", "θ": "TH",
      "œ": "E", "ɹ": "RR", "ɾ": "DD", "ɻ": "RR", "ʁ": "RR", "ɽ": "RR",
      "ʂ": "SS", "ʃ": "SS", "ʈ": "DD", "ʧ": "CH", "ʊ": "U", "ʋ": "FF",
      "ʌ": "aa", "ɣ": null, "ɤ": "O", "χ": null, "ʎ": "RR", "ʒ": "SS",
      "ʔ": null, "ˈ": null, "ˌ": null, "ː": null, "ʰ": null, "ʲ": null,
      "↓": null, "→": null, "↗": null, "↘": null, "ᵻ": "I"
    };

    // Allowed letters in upper case
    // NOTE: Diacritics will be removed unless added to this object.
    this.normalizedLettersUpper = {
      'A': 'A', 'B': 'B', 'C': 'C', 'D': 'D', 'E': 'E', 'F': 'F', 'G': 'G',
      'H': 'H', 'I': 'I', 'J': 'J', 'K': 'K', 'L': 'L', 'M': 'M', 'N': 'N',
      'O': 'O', 'P': 'P', 'Q': 'Q', 'R': 'R', 'S': 'S', 'T': 'T', 'U': 'U',
      'V': 'V', 'W': 'W', 'X': 'X', 'Y': 'Y', 'Z': 'Z', 'ß': 'SS', 'Ø': 'O',
      'Æ': 'AE', 'Œ': 'OE', 'Ð': 'D', 'Þ': 'TH', 'Ł': 'L'
    };

    // Allowed punctuations
    this.punctuations = {
      ";": ";", ":": ":", ",": ",", ".": ".", "!": "!", "?": "?", "¡": "!",
      "¿": "?", "—": "—", '"': '"', "…": "…", "«": '"', "»": '"', "“": '"',
      "”": '"', "(": "(", ")": ")", "{": "(", "}": ")", "[": "(", "]": ")",
      " ": " ", "-": "-", "'": "'"
    };

    // Allowed punctuations in mid-word
    this.punctuationsMidWord = {
      "-": "-", "'": "'", ".": ".", ",": ","
    }

    // Grapheme segmenter
    this.segmenter = new Intl.Segmenter('en', { granularity: 'grapheme' });

    if ( this.settings.trace ) {
      utils.trace( 'Language base module initiated.' );
    }
  }

  /**
  * Convert graphemes to phonemes.
  *
  * @param {string} s Word
  * @return {string[]} Array of phonemes
  */
  phonemizeWord(s) {
    throw new Error("The method phonemizeWord not implemented.");
  }

  /**
  * Add one dictionary line.
  *
  * @param {string} s Line
  */
  addToDictionary(s) {
    if ( s.startsWith(";;;") ) return; // Comment
    const fields = s.split("\t");
    if ( fields.length >= 2) {
      const word = fields[0];
      const phonemes = fields[1].split("");
      this.dictionary[word] = phonemes;
    }
  }

  /**
  * Load pronouncing dictionary.
  *
  * @param {string} [dictionary=null] Dictionary path/url. If null, do not use dictionaries
  * @param {boolean} [force=false] If true, re-load even if already loaded.
  */

  /**
  * Split text string in parts that contain one word.
  * Each part is split from the first letter of the word, except
  * the first part, which starts from the beginning including
  * leading spaces, if any.
  *
  * @param {string} s Text string
  * @return {string[]} Array of parts
  */
  splitText(s) {

    const parts = [];
    const chars = Array.from(this.segmenter.segment(s), seg => seg.segment);
    const len = chars.length;
    let i = 0;
    let lastType = 0; // 0=unknown, 1=whitespace, 2=other
    let foundWord = false;
    let part = "";
    while( i<len ) {
      const isLast = i === (len-1);
      const c = chars[i];
      const type = this.whitespaces.hasOwnProperty(c) ? 1 : 2;

      if ( isLast ) {
        parts.push(part+c);
      } else if ( foundWord && type === 2 && type !== lastType) {
        parts.push(part);
        part = c;
        lastType = type;
        foundWord = type === 2;
      } else {
        part += c;
        lastType = type;
        foundWord = foundWord || type === 2;
      }

      i++;
    }
    return parts;

  }

  /**
  * Normalize text and set it to uppercase.
  *
  * @param {string} s Text string
  * @return {string[]} Normalized array of characters in upper case.
  */
  normalizeUpper(s) {
    const norm = [];
    const chars = Array.from(this.segmenter.segment(s.toUpperCase()), seg => seg.segment);
    const len = chars.length;
    let i=0;
    while( i<len ) {
      let c = chars[i];
      if ( this.normalizedLettersUpper.hasOwnProperty(c) ) {
        norm.push( this.normalizedLettersUpper[c] );
      } else if ( this.punctuations.hasOwnProperty(c) ) {
        norm.push( this.punctuations[c] );
      } else {
        c = c.normalize("NFD").replace(/[\u0300-\u036f]/g, "").normalize("NFC");
        if ( this.normalizedLettersUpper.hasOwnProperty(c) ) {
          norm.push( this.normalizedLettersUpper[c] );
        }
      }
      i++;
    }
    return norm;
  }

  /**
  * Set the `text` to be spoken by analysing the part content.
  * NOTE: The language module should override this
  * method and implement language specific conversions.
  *
  * @param {Object} part Current part
  * @param {number} i Index
  * @param {Object[]} arr All the parts.
  */
  partSetText(part,i,arr) {

    switch( part.type ) {
      case "text":
        part.text = part.value;
        part.subtitles = part.value;
        break;

      case "speech":
        part.text = part.value;
        break;

      case "phonetic":
        part.phonemes = part.value;
        break;

      case "break":
        part.phonemes = " ";
        part.silence = part.value;
        break;

      // Leave other types to language specific implementation
    }
    part.subtitles = part.subtitles || "";

  }


  /**
  * Divide the given part into punctionations and words.
  * NOTE: Only `text` type currently supported.
  * TODO: Add support for other types.
  *
  * @param {Object} part Part object
  */
  partSetSpeak(part) {

    part.speak = [];

    if ( part.text && typeof part.text === "string" ) {
      const chars = this.normalizeUpper(part.text);
      const len = chars.length;
      let i=0;
      let word = "";
      let lastType = 0; // 0=Unknown, 1=punctuation, 2=word
      while( i<len ) {
        const isLast = i === (len-1);
        const c = chars[i];
        const cNext = isLast ? null : chars[i+1];
        const isPunctuation = this.punctuations.hasOwnProperty(c);
        const isPunctuationMid = !isLast && this.punctuationsMidWord.hasOwnProperty(c) && !this.punctuations.hasOwnProperty(cNext);
        const type = (isPunctuation && !isPunctuationMid) ? 1 : 2;

        if ( type === lastType ) {
          word += c;
        } else {
          if ( word ) {
            part.speak.push({ type: lastType, word: word });
            word = "";
          }
          word = c;
          lastType = type;
        }
        if ( isLast && word ) {
          part.speak.push({ type, word });
        }
        i++;
      }
    }

  }

  /**
  * Construct `phonemes` from speakable words and punctuations.
  *
  * @param {Object} part Part object
  */
  partSetPhonemes(part) {

    // Initialize
    part.phonemes = [];

    if ( part.speak && Array.isArray(part.speak) ) {
      const len = part.speak.length;
      let i = 0;
      while ( i<len ) {
        const s = part.speak[i];
        if ( s.type === 1 ) {
          part.phonemes.push(...s.word);
        } else if ( s.type === 2 ) {
          part.phonemes.push(...this.phonemizeWord(s.word));
        }
        i++;
      }
    }

  }

  /**
  * Construct `visemes` from `phonemes`.
  *
  * @param {Object} part Part object
  */
  partSetVisemes(part) {

    // Initialize
    part.visemes = [];

    // Map
    if ( part.phonemes && Array.isArray(part.phonemes)) {
      part.phonemes.forEach( ph => {
        if ( this.misakiToOculusViseme.hasOwnProperty(ph) ) {
          part.visemes.push( this.misakiToOculusViseme[ph] );
        } else {
          // console.info('Viseme not found for "' + ph + '"');
          part.visemes.push(null)
        }
      });
    }

  }


  /**
  * Generate phonemes and TalkingHead metadata from input.
  * NOTE: Starting times and durations in `metadata` refer to phoneme
  * start and end indices respectively.
  *
  * @param {string|Object[]} input Input element
  * @return {Object} `phonemes` array, TalkingHead `metadata` template, and `silences`
  */
  generate(input) {

    // Output data
    let phonemes = [];
    const metadata = {
      phonemes: [],
      words: [], wtimes: [], wdurations: [],
      visemes: [], vtimes: [], vdurations: []
    };
    const silences = [];

    if ( this.settings.trace ) {
      utils.trace( 'Generate start, input=', input );
    }

    // Break input into parts
    let parts = [];
    const inputs = Array.isArray(input) ? input : [input];
    inputs.forEach( x => {
      if ( typeof x === "string" ) {
        const textParts = this.splitText(x);
        textParts.forEach( y => {
          const part = { type: "text", value: y };
          parts.push( part );
        });
      } else {
        parts.push(x);
      }
    });

    // Set text to be spoken
    parts.forEach( this.partSetText.bind(this) );

    // Populate output
    parts.forEach( part => {

      // Phonemize and set visemes
      if ( part.hasOwnProperty("phonemes") ) {
        if ( typeof part.phonemes === "string" ) {
          part.phonemes = [...part.phonemes];
        }
      } else {
        this.partSetSpeak(part);
        this.partSetPhonemes(part);
      }

      // Set visemes
      this.partSetVisemes(part);

      if ( this.settings.trace ) {
        utils.trace( 'Generate, part=', part );
      }

      // Set output
      metadata.words.push( part.subtitles );
      metadata.wtimes.push( phonemes.length );
      const len = part.phonemes.length
      let i = phonemes.length; // Phoneme index
      for( let j=0; j<len; j++ ) {

        // Phonemes
        const ph = part.phonemes[j];
        phonemes.push( ph );

        // Visemes
        const viseme = part.visemes[j];
        if ( viseme ) {
          metadata.phonemes.push(ph);
          metadata.visemes.push(viseme);
          metadata.vtimes.push(i);
          i = phonemes.length;
          metadata.vdurations.push(i);
        } else {

          // Custom timing
          if ( ph === "ˈ" || ph === "ˌ" ) {
            // Do not update startingtime
          } else if ( ph === "ː" ) {
            i = phonemes.length;
            const len2 = metadata.visemes.length;
            if ( len2 ) {
              metadata.vdurations[len2-1] = i;
            }
          } else {
            i = phonemes.length;
          }

        }
      }
      metadata.wdurations.push( phonemes.length );
      if ( part.hasOwnProperty("silence") ) {
        silences.push( [ phonemes.length, part.silence ] );
      }

    });

    if ( this.settings.trace ) {
      utils.trace( 'Generate end, output=', {
        phonemes: utils.deepCopy(phonemes),
        metadata: utils.deepCopy(metadata),
        silences: utils.deepCopy(silences)
      });
    }

    // Output
    return { phonemes, metadata, silences };

  }

}

export { LanguageBase };
