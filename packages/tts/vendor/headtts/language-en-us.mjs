// Adapted from @met4citizen/headtts 1.3.0; Copyright 2025 Mika Suominen, MIT.
// VideoCut 2026-10-01: remove dictionary I/O and unused runtime utilities; dictionaries are bundled offline.
import { LanguageBase } from "./language.mjs"
import * as utils from "./utils.mjs";

/**
* @class American English language module
* @author Mika Suominen
*/

class Language extends LanguageBase {

  /**
  * @constructor
  */
  constructor( settings = null ) {
    super(settings);

    // OVERRIDE FROM BASE CLASS:
    // Allowed letters in upper case
    // NOTE: Diacritics will be removed unless added to this object.
    this.normalizedLettersUpper = {
      'A': 'A', 'B': 'B', 'C': 'C', 'D': 'D', 'E': 'E', 'F': 'F', 'G': 'G',
      'H': 'H', 'I': 'I', 'J': 'J', 'K': 'K', 'L': 'L', 'M': 'M', 'N': 'N',
      'O': 'O', 'P': 'P', 'Q': 'Q', 'R': 'R', 'S': 'S', 'T': 'T', 'U': 'U',
      'V': 'V', 'W': 'W', 'X': 'X', 'Y': 'Y', 'Z': 'Z', 'ß': 'SS', 'Ø': 'O',
      'Æ': 'AE', 'Œ': 'OE', 'Ð': 'D', 'Þ': 'TH', 'Ł': 'L'
    };

    // English words to phonemes, algorithmic rules adapted from:
    //   NRL Report 7948, "Automatic Translation of English Text to Phonetics by Means of Letter-to-Sound Rules" (1976)
    //   by HONEY SUE ELOVITZ, RODNEY W. JOHNSON, ASTRID McHUGH, AND JOHN E. SHORE
    //   Available at: https://apps.dtic.mil/sti/pdfs/ADA021929.pdf
    this.rules = {
      'A': [
        "[A] =AX", " [ARE] =AA R", " [AR]O=AX R", "[AR]#=EH R",
        " ^[AS]#=EY S", "[A]WA=AX", "[AW]=AO", " :[ANY]=EH N IY",
        "[A]^+#=EY", "#:[ALLY]=AX L IY", " [AL]#=AX L", "[AGAIN]=AX G EH N",
        "#:[AG]E=IH JH", "[A]^+:#=AE", ":[A]^+ =EY", "[A]^%=EY",
        " [ARR]=AX R", "[ARR]=AE R", " :[AR] =AA R", "[AR] =ER",
        "[AR]=AA R", "[AIR]=EH R", "[AI]=EY", "[AY]=EY", "[AU]=AO",
        "#:[AL] =AX L", "#:[ALS] =AX L Z", "[ALK]=AO K", "[AL]^=AO L",
        " :[ABLE]=EY B AX L", "[ABLE]=AX B AX L", "[ANG]+=EY N JH", "[A]=AE"
      ],

      'B': [
        " [BE]^#=B IH1", "[BEING]=B IH", " [BOTH] =B OW1 TH",
        " [BUS]#=B IH1 Z", "[BUIL]=B IH L", "[B]=B"
      ],

      'C': [
        " [CH]^=K", "^E[CH]=K", "[CH]=CH", " S[CI]#=S AY1",
        "[CI]A=SH", "[CI]O=SH", "[CI]EN=SH", "[C]+=S",
        "[CK]=K", "[COM]%=K AH M", "[C]=K"
      ],

      'D': [
        "#:[DED] =D IH D", ".E[D] =D", "#^:E[D] =T", " [DE]^#=D IH1",
        " [DO] =D UW1", " [DOES]=D AH Z", " [DOING]=D UW IH NX",
        " [DOW]=D AW1", "[DU]A=JH UW1", "[D]=D"
      ],

      'E': [
        "#:[E] =", "'^:[E] =", " :[E] =IY", "#[ED] =D", "#:[E]D =",
        "[EV]ER=EH V", "[E]^%=IY", "[ERI]#=IY R IY", "[ERI]=EH R IH",
        "#:[ER]#=ER", "[ER]#=EH R", "[ER]=ER", " [EVEN]=IY V EH N",
        "#:[E]W=", "@[EW]=UW", "[EW]=Y UW", "[E]O=IY", "#:&[ES] =IH Z",
        "#:[E]S =", "#:[ELY] =L IY", "#:[EMENT]=M EH N T", "[EFUL]=F UH L",
        "[EE]=IY", "[EARN]=ER N", " [EAR]^=ER", "[EAD]=EH D", "#:[EA] =IY AX",
        "[EA]SU=EH", "[EA]=IY", "[EIGH]=EY", "[EI]=IY", " [EYE]=AY", "[EY]=IY",
        "[EU]=Y UW", "[E]=EH"
      ],

      'F': [
        "[FUL]=F UH1 L", "[F]=F"
      ],

      'G': [
        "[GIV]=G IH V", " [G]I^=G", "[GE]T=G EH", "SU[GGES]=G JH EH S",
        "[GG]=G", " B#[G]=G", "[G]+=JH", "[GREAT]=G R EY T",
        "#[GH]=", "[G]=G"
      ],

      'H': [
        " [HAV]=HH AE V", " [HERE]=HH IY R", " [HOUR]=AW ER", "[HOW]=HH AW",
        "[H]#=HH", "[H]="
      ],

      'I': [
        " [IN]=IH N", " [I] =AY", "[IN]D=AY N", "[IER]=IY ER",
        "#:R[IED] =IY D", "[IED] =AY D", "[IEN]=IY EH N", "[IE]T=AY EH",
        " :[I]%=AY", "[I]%=IY", "[IE]=IY", "[I]^+:#=IH", "[IR]#=AY R",
        "[IZ]%=AY Z", "[IS]%=AY Z", "[I]D%=AY", "+^[I]^+=IH",
        "[I]T%=AY", "#^:[I]^+=IH", "[I]^+=AY", "[IR]=ER", "[IGH]=AY",
        "[ILD]=AY L D", "[IGN] =AY N", "[IGN]^=AY N", "[IGN]%=AY N",
        "[IQUE]=IY K", "[I]=IH"
      ],

      'J': [
        "[J]=JH"
      ],

      'K': [
        " [K]N=", "[K]=K"
      ],

      'L': [
        "[LO]C#=L OW1", "L[L]=", "#^:[L]%=AX L", "[LEAD]=L IY D", "[L]=L"
      ],

      'M': [
        "[MOV]=M UW V", "[M]=M"
      ],

      'N': [
        "E[NG]+=N JH", "[NG]R=NX G", "[NG]#=NX G", "[NGL]%=NX G AX L",
        "[NG]=NX", "[NK]=NX K", " [NOW] =N AW1", "[N]=N"
      ],

      'O': [
        "[OF] =AX V", "[OROUGH]=ER OW", "#:[OR] =ER", "#:[ORS] =ER Z",
        "[OR]=AO R", " [ONE]=W AH N", "[OW]=OW", " [OVER]=OW V ER",
        "[OV]=AH V", "[O]^%=OW", "[O]^EN=OW", "[O]^I#=OW", "[OL]D=OW L",
        "[OUGHT]=AO T", "[OUGH]=AH F", " [OU]=AW", "H[OU]S#=AW",
        "[OUS]=AX S", "[OUR]=AO R", "[OULD]=UH D", "^[OU]^L=AH",
        "[OUP]=UW P", "[OU]=AW", "[OY]=OY", "[OING]=OW IH NX", "[OI]=OY",
        "[OOR]=AO R", "[OOK]=UH K", "[OOD]=UH D", "[OO]=UW", "[O]E=OW",
        "[O] =OW", "[OA]=OW", " [ONLY]=OW N L IY", " [ONCE]=W AH N S",
        "[ON'T]=OW N T", "C[O]N=AA", "[O]NG=AO", " ^:[O]N=AH",
        "I[ON]=AX N", "#:[ON] =AX N", "#^[ON]=AX N", "[O]ST =OW",
        "[OF]^=AO F", "[OTHER]=AH DH ER", "[OSS] =AO S", "#^:[OM]=AH M",
        "[O]=AA"
      ],

      'P': [
        "[PH]=F", "[PEOP]=P IY1 P", "[POW]=P AW1", "[PUT] =P UH1 T",
        "[P]=P"
      ],

      'Q': [
        "[QUAR]=K W AO R", "[QU]=K W", "[Q]=K"
      ],

      'R': [
        " [RE]^#=R IY", "[R]=R"
      ],

      'S': [
        "[SH]=SH", "#[SION]=ZH AX N", "[SOME]=S AH M", "#[SUR]#=ZH ER",
        "[SUR]#=SH ER", "#[SU]#=ZH UW", "#[SSU]#=SH UW", "#[SED] =Z D",
        "#[S]#=Z", "[SAID]=S EH D", "^[SION]=SH AX N", "[S]S=",
        ".[S] =Z", "#:.E[S] =Z", "#^:##[S] =Z", "#^:#[S] =S",
        "U[S] =S", " :#[S] =Z", " [SCH]=S K", "[S]C+=",
        "#[SM]=Z M", "#[SN]'=Z AX N", "[S]=S"
      ],

      'T': [
        " [THE] =DH AX", "[TO] =T UW", "[THAT] =DH AE T", " [THIS] =DH IH S",
        " [THEY]=DH EY", " [THERE]=DH EH R", "[THER]=DH ER", "[THEIR]=DH EH R",
        " [THAN] =DH AE N", " [THEM] =DH EH M", "[THESE] =DH IY Z",
        " [THEN]=DH EH N", "[THROUGH]=TH R UW", "[THOSE]=DH OW Z",
        "[THOUGH] =DH OW", " [THUS]=DH AH S", "[TH]=TH", "#:[TED] =T IH D",
        "S[TI]#N=CH", "[TI]O=SH", "[TI]A=SH", "[TIEN]=SH AX N",
        "[TUR]#=CH ER", "[TU]A=CH UW", " [TWO]=T UW", "[T]=T"
      ],

      'U': [
        " [UN]I=Y UW N", " [UN]=AH N", " [UPON]=AX P AO N",
        "@[UR]#=UH R", "[UR]#=Y UH R", "[UR]=ER", "[U]^ =AH",
        "[U]^^=AH", "[UY]=AY", " G[U]#=", "G[U]%=", "G[U]#=W",
        "#N[U]=Y UW", "@[U]=UW", "[U]=Y UW"
      ],

      'V': [
        "[VIEW]=V Y UW", "[V]=V"
      ],

      'W': [
        " [WERE]=W ER", "[WA]S=W AA", "[WA]T=W AA", "[WHERE]=WH EH R",
        "[WHAT]=WH AA T", "[WHOL]=HH OW L", "[WHO]=HH UW", "[WH]=WH",
        "[WAR]=W AO R", "[WOR]^=W ER", "[WR]=R", "[W]=W"
      ],

      'X': [
        " [X]=S", "[X]=K S"
      ],

      'Y': [
        "[YOUNG]=Y AH NX", " [YOU]=Y UW", " [YES]=Y EH S", " [Y]=Y",
        "#^:[Y] =IY", "#^:[Y]I=IY", " :[Y] =AY", " :[Y]#=AY",
        " :[Y]^+:#=IH", " :[Y]^#=AY", "[Y]=IH"
      ],

      'Z': [
        "[Z]=Z"
      ]
    };

    const ops = {
      '#': '[AEIOUY]+', // One or more vowels AEIOUY
      // This one is not used: "'": '[BCDFGHJKLMNPQRSTVWXZ]+', // One or more consonants BCDFGHJKLMNPQRSTVWXZ
      '.': '[BDVGJLMNRWZ]', // One voiced consonant BDVGJLMNRWZ
      // This one is not used: '$': '[BDVGJLMNRWZ][EI]', // One consonant followed by E or I
      '%': '(?:ER|E|ES|ED|ING|ELY)', // One of ER, E, ES, ED, ING, ELY
      '&': '(?:[SCGZXJ]|CH|SH)', // One of S, C, G, Z, X, J, CH, SH
      '@': '(?:[TSRDLZNJ]|TH|CH|SH)', // One of T, S, R, D, L, Z, N, J, TH, CH, SH
      '^': '[BCDFGHJKLMNPQRSTVWXZ]', // One consonant BCDFGHJKLMNPQRSTVWXZ
      '+': '[EIY]', // One of E, I, Y
      ':': '[BCDFGHJKLMNPQRSTVWXZ]*', // Zero or more consonants BCDFGHJKLMNPQRSTVWXZ
      ' ': '\\b' // Start/end of the word
    };

    // Reference: https://en.wikipedia.org/wiki/Arpabet
    const ArpabetToIPA = {
      // Rules
			'AO': 'ɔ', 'AA': 'ɑ', 'IY': 'i', 'UW': 'u', 'EH': 'ɛ', 'IH': 'ɪ',
			'UH': 'ʊ', 'AH': 'ə', 'AE': 'æ', 'AX': 'ə', 'EY': 'eɪ', 'AY': 'aɪ',
			'OW': 'oʊ', 'AW': 'aʊ', 'OY': 'ɔɪ', 'P': 'p', 'B': 'b', 'T': 't',
			'D': 'd', 'K': 'k', 'G': 'ɡ', 'CH': 'tʃ', 'JH': 'dʒ', 'F': 'f',
      'V': 'v', 'TH': 'θ', 'DH': 'ð', 'S': 's', 'Z': 'z', 'SH': 'ʃ',
			'ZH': 'ʒ', 'HH': 'h', 'M': 'm', 'N': 'n', 'NX': 'ŋ', 'NG': 'ŋ',
      'L': 'l', 'R': 'ɹ', 'ER': 'ɚ', 'W': 'w', 'WH': 'w', 'Y': 'j',

      // CMU dictionary stressed vowels
      "AA0": "ɑ", "AA1": "ˈɑ", "AA2": "ˌɑ", "AE0": "æ", "AE1": "ˈæ",
      "AE2": "ˌæ", "AH0": "ə", "AH1": "ˈʌ", "AH2": "ˌʌ", "AO0": "ɔ",
      "AO1": "ˈɔ", "AO2": "ˌɔ", "AW0": "aʊ", "AW1": "ˈaʊ", "AW2": "ˌaʊ",
      "AY0": "aɪ",  "AY1": "ˈaɪ",  "AY2": "ˌaɪ", "EH0": "ɛ", "EH1": "ˈɛ",
      "EH2": "ˌɛ", "ER0": "ɚ", "ER1": "ˈɝ", "ER2": "ˌɝ", "EY0": "eɪ",
      "EY1": "ˈeɪ",  "EY2": "ˌeɪ", "IH0": "ɪ", "IH1": "ˈɪ", "IH2": "ˌɪ",
      "IY0": "i", "IY1": "ˈi", "IY2": "ˌi", "OW0": "oʊ", "OW1": "ˈoʊ",
      "OW2": "ˌoʊ", "OY0": "ɔɪ", "OY1": "ˈɔɪ", "OY2": "ˌɔɪ", "UH0": "ʊ",
      "UH1": "ˈʊ", "UH2": "ˌʊ", "UW0": "u", "UW1": "ˈu", "UW2": "ˌu"
    };

    const IPAToMisaki = {
      "ɚ": ["ɜ","ɹ"], "ˈɝ": ["ˈɜ","ɹ"], "ˌɝ": ["ˌɜ","ɹ"],
      "tʃ": ["ʧ"], "dʒ": ["ʤ"],
      "eɪ": ["A"], "ˈeɪ": ["ˈA"], "ˌeɪ": ["ˌA"],
      "aɪ": ["I"], "ˈaɪ": ["ˈI"], "ˌaɪ": ["ˌI"],
      "aʊ": ["W"], "ˈaʊ": ["ˈW"], "ˌaʊ": ["ˌW"],
      "ɔɪ": ["Y"], "ˈɔɪ": ["ˈY"], "ˌɔɪ": ["ˌY"],
      "oʊ": ["O"], "ˈoʊ": ["ˈO"], "ˌoʊ": ["ˌO"],
      "əʊ": ["Q"], "ˈəʊ": ["ˈQ"], "ˌəʊ": ["ˌQ"]
    };

    // Convert rules to regex
    Object.keys(this.rules).forEach( key =>  {
      this.rules[key] = this.rules[key].map( rule =>  {
        const posL = rule.indexOf('[');
        const posR = rule.indexOf(']');
        const posE = rule.indexOf('=');
        const strLeft = rule.substring(0,posL);
        const strLetters = rule.substring(posL+1,posR);
        const strRight = rule.substring(posR+1,posE);
        const strPhonemes = rule.substring(posE+1);

        const o = { regex: '', move: 0, phonemes: [] };

        let exp = '';
        exp += [...strLeft].map( x => ops[x] || x ).join('');
        const ctxLetters = [...strLetters];
        ctxLetters[0] = ctxLetters[0].toLowerCase();
        exp += ctxLetters.join('');
        o.move = ctxLetters.length;
        exp += [...strRight].map( x => ops[x] || x ).join('');
        o.regex = new RegExp(exp);

        if ( strPhonemes.length ) {
          strPhonemes.split(' ').forEach( ph =>  {
            const ipa = ArpabetToIPA[ph];
            if ( IPAToMisaki.hasOwnProperty(ipa) ) {
              o.phonemes.push( ...IPAToMisaki[ipa] );
            } else {
              o.phonemes.push( ipa );
            }
          });
        }

        return o;
      });
    });

    // Characters to phonemes
    this.charactersToPhonemes = {
      '!':	"ˌɛkskləmˈAʃənpˌYnt", '"': "kwˈOt", '#': "pˈWndsˌIn", '%': "pɜɹsˈɛnt",
      '&': "ˈæmpɜɹsˌænd", "'": "əpˈɑstɹəfi", '(': "ˈOpənpɜɹˈɛnθəsˌiz",
      ')': "klˈOzpɜɹˈɛnθəsˌiz", '+': "plˈʊs", '-': 'dˈæʃ', '—': 'dˈæʃ', ',': "kˈɑmə",
      '.': "dˈɑt", '/': "slˈæʃ", ':': "kˈOlən", ';': "sˈɛmikˈOlən", '?': "kwˈɛsʧənmˈɑɹk",
      'A': "ˈə", 'B': "bˈi", 'C': "sˈi", 'D': "dˈi", 'E': "ˈi", 'F': "ˈɛf",
      'G': "ʤˈi", 'H': "ˈAʧ", 'I': "I", 'J': "ʤˈA", 'K': "kˈA", 'L': "ˈɛl",
      'M': "ˈɛm", 'N': "ˈɛn", 'O': "ˈO", 'P': "pˈi", 'Q': "kjˈu", 'R': "ˈɑɹ",
      'S': "ˈɛs", 'T': "tˈi", 'U': "jˈu", 'V': "vˈi", 'W': "dˈʌbəlju",
      'X': "ˈɛks", 'Y': "wˈI", 'Z': "zˈi", '1': "wˈʌn", '2': "tˈu", '3': "θɹˈi",
      '4': "fˈɔɹ", '5': "fˈIv", '6': "sˈɪks", '7': "sˈɛvən", '8': "ˈAt",
      '9': "nˈIn", '0': "zˈiɹO", '{': "ˈOpɛnbɹˈAs", '}': "klˈOzbɹˈAs",
      '$': "dˈɑlɜɹ", '€': "jˈuɹO"
    };

    // English number words
    this.digits = ['OH', 'ONE', 'TWO', 'THREE', 'FOUR', 'FIVE', 'SIX', 'SEVEN', 'EIGHT', 'NINE'];
    this.ones = ['','ONE','TWO','THREE','FOUR','FIVE','SIX','SEVEN','EIGHT','NINE'];
    this.tens = ['','','TWENTY','THIRTY','FORTY','FIFTY','SIXTY','SEVENTY','EIGHTY','NINETY'];
    this.teens = ['TEN','ELEVEN','TWELVE','THIRTEEN','FOURTEEN','FIFTEEN','SIXTEEN','SEVENTEEN','EIGHTEEN','NINETEEN'];
    this.decades = {
      20: "TWENTIES", 30: "THIRTIES", 40: "FORTIES", 50: "FIFTIES",
      60: "SIXTIES", 70: "SEVENTIES", 80: "EIGHTIES", 90: "NINETIES"
    };
    this.ordinals = {
      1: "FIRST", 2: "SECOND", 3: "THIRD", 4: "FOURTH", 5: "FIFTH",
      6: "SIXTH", 7: "SEVENTH", 8: "EIGHTH", 9: "NINTH", 10: "TENTH",
      11: "ELEVENTH", 12: "TWELFTH", 13: "THIRTEENTH", 14: "FOURTEENTH",
      15: "FIFTEENTH", 16: "SIXTEENTH", 17: "SEVENTEETH", 18: "EIGHTEENTH",
      19: "NINETEENTH", 20: "TWENTIETH", 30: "THIRTIETH", 40: "FORTIETH",
      50: "FIFTIETH", 60: "SIXTIETH",70: "SEVENTIETH", 80: "EIGHTIETH",
      90: "NINETIETH"
    };

    // Date & Time
    this.months = [
      "", "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
      "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    ];
    this.days = [
      "", "FIRST", "SECOND", "THIRD", "FOURTH", "FIFTH", "SIXTH", "SEVENTH", "EIGHT", "NINTH", "TENTH",
      "ELEVENTH", "TWELFTH", "THIRTEENTH", "FOURTEENTH", "FIFTEENTH", "SIXTEENTH", "SEVENTEETH",
      "EIGHTEENTH", "NINETEENTH", "TWENTIETH", "TWENTY-FIRST", "TWENTY-SECOND", "TWENTY-THIRD",
      "TWENTY-FOURTH", "TWENTY-FIFTH", "TWENTY-SIXTH", "TWENTY-SEVENTH", "TWENTY-EIGHT", "TWENTY-NINTH", "THIRTIETH",
      "THIRTY-FIRST"
    ];

    // Symbols to English
    // TODO: Implement
    this.symbols = {
      '%': 'PERCENT', '€': 'EUROS', '&': 'AND', '+': 'PLUS',
      '$': 'DOLLARS'
    };

    this.symbolsReg = /[%€&\+\$]/g;

    if ( this.settings.trace ) {
      utils.trace( 'Language module "en-us" initiated.' );
    }
  }


  /**
  * Read number digit-by-digit.
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertDigitByDigit(num) {
    num = String(num).split("");
    let numWords = "";
    for(let m=0; m<num.length; m++) {
      numWords += this.digits[num[m]] + " ";
    }
    numWords = numWords.substring(0, numWords.length - 1); //kill final space
    return numWords;
  }

  /**
  * Read number in sets of two (year).
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertSetsOfTwo(num) {
    let firstNumHalf = String(num).substring(0, 2);
    let secondNumHalf = String(num).substring(2, 4);
    let numWords = this.convertTens(firstNumHalf);
    numWords += " " + this.convertTens(secondNumHalf);
    return numWords;
  }

  /**
  * Read millions.
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertMillions(num){
    if (num>=1000000){
      return this.convertMillions(Math.floor(num/1000000))+" MILLION "+this.convertThousands(num%1000000);
    } else {
      return this.convertThousands(num);
    }
  }

  /**
  * Read thousands.
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertThousands(num){
    if (num>=1000){
      return this.convertHundreds(Math.floor(num/1000))+" THOUSAND "+this.convertHundreds(num%1000);
    } else {
      return this.convertHundreds(num);
    }
  }

  /**
  * Read hundreds.
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertHundreds(num){
    if (num>99){
      return this.ones[Math.floor(num/100)]+" HUNDRED "+this.convertTens(num%100);
    } else {
      return this.convertTens(num);
    }
  }

  /**
  * Read tens.
  *
  * @param {number|string} num Number
  * @return {string} String
  */
  convertTens(num){
    if (num < 10){
      return (Number(num) != 0 && num.toString().startsWith("0") ? "OH " : "") + this.ones[Number(num)];
    } else if (num>=10 && num<20) {
      return this.teens[num-10];
    } else {
      return (this.tens[Math.floor(num/10)]+" "+this.ones[num%10]).trim();
    }
  }

  /**
  * Convert number to words. Try to decide how to read it.
  *
  * @param {number|string} num Number
  * @param {boolean} [isNotSpecial=false] If true, this is not a special number (e.g. year, zip code)
  * @return {string} String
  */
  convertNumberToWords(num, isNotSpecial=false) {
    const n = parseFloat(num);
    if (num == "0") {
      return "ZERO";
    } else if (num < 0 ) {
      return "MINUS " + this.convertNumberToWords( Math.abs(num).toString(), isNotSpecial ).trim();
    } else if ( n && !Number.isInteger(n) ) {
      const parts = n.toString().split('.');
      return this.convertNumberToWords(parts[0], isNotSpecial).trim() + " POINT " + this.convertDigitByDigit(parts[1]).trim();
    } else if(num.toString().startsWith('0')){
      return this.convertDigitByDigit(num).trim();
    } else if (!isNotSpecial && ((num<1000 && num>99 && (num % 100) !== 0) || (num>10000&&num<1000000))) { //read area and zip codes digit by digit
      return this.convertDigitByDigit(num).trim();
    } else if (!isNotSpecial && ((num > 1000 && num < 2000)||(num>2009&&num<3000))) { //read years as two sets of two digits
      return (num % 100 != 0 ? this.convertSetsOfTwo(num).trim() : this.convertTens(num.toString().substring(0, 2)).trim() + " HUNDRED");
    } else {
      return this.convertMillions(num).trim();
    }
  }

  /**
  * Expand decade to text.
  *
  * @param {string} decade Decade
  * @return {string} Normalized text
  */
  convertDecade(decade) {
    const num = parseInt(decade);
    const isShort = !isNaN(num) && decade.length === 2;
    const isLong = !isNaN(num) && decade.length > 2 && num > 0 && num <= 3000;
    const thousands = (isLong && (num % 1000) === 0 ) ? Math.floor(num / 1000) : null;
    const hundreds = (isLong && !thousands) ?  Math.floor(num / 100) : null;
    const tens = (isShort || isLong) ? Math.floor((num % 100) / 10) * 10 : null;

    let s = [];
    if ( thousands ) {
      s.push( this.convertNumberToWords(thousands).trim(), "THOUSANDS" );
    } else {
      if ( hundreds ) {
        s.push( this.convertNumberToWords(hundreds).trim() );
      }
      if ( tens ) {
        s.push( this.decades[tens] || (this.convertNumberToWords(tens).trim() + 'S') );
      } else if ( hundreds ) {
        s.push( "HUNDREDS" );
      } else {
        s.push( decade );
      }
    }

    return s.join(" ");
  }

  /**
  * Convert ordinal number to text.
  *
  * @param {number} num Ordinal number
  * @return {string} Normalized text
  */
  convertOrdinal(num) {

    // Return immediately, if we have the number in our map
    if ( this.ordinals.hasOwnProperty(num) ) {
      return this.ordinals[num];
    }

    const hundreds = Math.floor(num / 100);
    const tens = Math.floor( (num % 100) / 10) * 10;
    const ones = num % 10;

    let s = [];
    if ( hundreds ) {
      s.push( this.convertNumberToWords(hundreds).trim() );
      if ( tens || ones ) {
        s.push( "HUNDRED" );
      } else {
        s.push( "HUNDREDTH" );
      }
    }

    if ( tens ) {
      if ( ones ) {
        s.push( this.convertNumberToWords(tens).trim() );
      } else {
        s.push( this.ordinals[tens] );
      }
    }

    if ( ones ) {
      s.push( this.ordinals[ones] );
    }

    return s.join(" ");
  }

  /**
  * Set the `text` to be spoken by analysing the part content.
  *
  * @param {Object} part Current part
  * @param {number} i Index
  * @param {Object[]} arr All the parts.
  */
  partSetText(part,i,arr) {
    
    // Call super to pre-populate
    super.partSetText(part,i,arr);

    // Language specific implementation
    switch( part.type ) {

      case "text":

        // Process numbers, if any
        if ( /\d/.test(part.text) ) {

          // Decades: 70s, 1970s -> SEVENTIES, NINETEEN SEVENTIES  
          part.text = part.text.replace(/\b(\d{2,4})[''']?\s?[sS](?=\s|[.,!?;:]|$)/g, (match, decade) => {
            const result = this.convertDecade(decade);
            return result === decade ? match : result;
          });

          // Ordinals: 1st, 22nd -> FIRST, TWENTY SECOND
          part.text = part.text.replace(/\b(\d+)\s*(st|nd|rd|th)(?=\s|[.,!?;:]|$)/gi, (match, number) => {
            return this.convertOrdinal(Number(number));
          });
          
          // Handle mixed alphanumeric sequences
          part.text = part.text.replace(/\b(\w*?)(\d+)([A-Za-z]+)\b/g, (match, prefix, numbers, letters) => {
            const processedNumber = this.convertNumberToWords(numbers);
            return `${prefix}${processedNumber} ${letters}`;
          }).replace(/\b([A-Za-z]+)(\d+)(\w*?)\b/g, (match, letters, numbers, suffix) => {
            const processedNumber = this.convertNumberToWords(numbers);
            return `${letters} ${processedNumber}${suffix}`;
          });

          // Process the remaining numbers
          // Note: If there are thousand separators or a decimal part, we know
          // that this is not a special number e.g. phone number, zip code or year
          part.text = part.text.replace(/-?(?:\d{1,3}(?:,\d{3})+|\d+)(\.\d+)?/g, (match, decimal) => {
            let s = match;
            let isNotSpecial = false;
            if ( /,/.test(s) ) {
              s = s.replace( /,/g, "" );
              isNotSpecial = true;
            }
            if ( decimal ) {
              isNotSpecial = true;
            }
            return this.convertNumberToWords(s, isNotSpecial);
          });

        }

        break;

      case "characters":
        const phonetic = [];
        const chars = [...part.value.toUpperCase()];
        const len = chars.length;
        for( let i=0; i<len; i++ ) {
          const c = chars[i];
          if ( this.charactersToPhonemes.hasOwnProperty(c) ) {
            phonetic.push( this.charactersToPhonemes[c] );
          } else {
            phonetic.push(""); // Generates a space for unknown characters
          }
        }
        part.phonemes = phonetic.join(" ") + " ";
        break;

      case "number":
        part.text = this.convertRegexNumberToWords(part.value) + " ";
        break;

      case "date":
        const date = new Date(part.value);
        const month = this.months[date.getMonth()];
        const day = this.days[date.getDate()];
        const year = this.convertSetsOfTwo(date.getFullYear());
        part.text = month + " " + day + ", " + year + " ";
        break;

      case "time":
        const time = new Date(part.value);
        let hours = time.getHours(); // 0–23
        const minutes = time.getMinutes(); // 0–59
        const ampm = hours >= 12 ? 'P M' : 'A M';
        hours = hours % 12;
        hours = hours ? hours : 12; // 0 becomes 12
        part.text = this.convertNumberToWords(hours) + " ";
        part.text += this.convertNumberToWords(minutes) + " ";
        part.text += ampm;
        break;
    }

  }

  /**
  * Convert graphemes to phonemes.
  *
  * @param {string} s Word, normalized and in upper case
  * @return {string[]} Array of phonemes
  */
  phonemizeWord(s) {

    const phonemes = [];

    // Dictionary lookup
    if ( this.dictionary ) {
      if ( this.dictionary.hasOwnProperty(s) ) {
        if ( this.settings.trace ) {
          utils.trace( 'Dictionary match: "' + s + '" => "' + this.dictionary[s].join("") + '"' );
        }
        phonemes.push( ...this.dictionary[s] );
        s = "";
      } else if ( s.length >= 5 ) {
        const len = s.length;
        for(let i=1; i<3; i++) {
          const part = s.substring(0,len-i);
          if ( this.dictionary.hasOwnProperty(part) ) {
            if ( this.settings.trace ) {
              utils.trace( 'Dictionary partial: "' + part + '" => "' + this.dictionary[part].join("") + '"' );
            }
            phonemes.push( ...this.dictionary[part] );
            s = s.substring(len-i);
            break;
          }
        }
      }
    }

    // If still not resolved, use rules
    if ( s.length ) {
      const firstPhoneme = phonemes.length;
      const chars = [...s];
      let len = chars.length;
      let i = 0;
      while( i < len ) {
        const c = chars[i];
        if ( this.punctuations.hasOwnProperty(c) ) {
          phonemes.push( this.punctuations[c] );
          i++;
        } else {
          const ruleset = this.rules[c];
          if ( ruleset ) {
            for(let j=0; j<ruleset.length; j++) {
              const rule = ruleset[j];
              const test = s.substring(0, i) + c.toLowerCase() + s.substring(i+1);
              let matches = test.match(rule.regex);
              if ( matches ) {
                rule.phonemes.forEach( ph =>  {
                  phonemes.push( ph );
                });
                i += rule.move;
                break;
              }
            }
          } else {
            i++;
          }
        }
      }
      if ( this.settings.trace ) {
        utils.trace( 'Rules: "' + s + '" => "' + phonemes.slice(firstPhoneme).join("") + '"' );
      }
    }

    return phonemes;
  }

}

export { Language };
