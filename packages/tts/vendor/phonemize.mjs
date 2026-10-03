// Derived from @uzen/kokoro-js 1.2.4, Apache-2.0.
// Source: https://registry.npmjs.org/@uzen/kokoro-js/-/kokoro-js-1.2.4.tgz
// Original dist/kokoro.js SHA256: 8d4826daa6a8a94360ebfa3062b6c491bd3831a83d58d6127fb9481a6f0bef49
// Modified: retain only Chinese/English phonemization; remove model loading, Node I/O and voice cache.
// Modified 2026-10-01: preserve signed quantities and decimal/version expressions before sentence punctuation.
// Modified 2026-10-01: replace compiled eSpeak dependency with offline permissive JavaScript English G2P.
import { phonemize as r } from './english.mjs';
import { pinyin as s } from 'pinyin-pro';
const o = new Map([
    ["，", ","],
    ["。", "."],
    ["！", "!"],
    ["？", "?"],
    ["；", ";"],
    ["：", ":"],
    ["、", ","],
  ]),
  c = ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九"],
  u = /^[ㄅ-ㄩ压言阳要阴应用又穵外万王为文瓮我中月元云ㄭ十]+[0-5]$/,
  h = /^[零一二三四五六七八九十百千万亿两]+$/,
  m = [
    "千万美元",
    "百万美元",
    "千万块",
    "百万块",
    "千万吨",
    "百万吨",
    "万美元",
    "亿元",
    "万元",
    "美元",
    "千克",
    "毫克",
    "微克",
    "公里",
    "公分",
    "公尺",
    "公寸",
    "公釐",
    "千米",
    "分米",
    "厘米",
    "毫米",
    "微米",
    "小时",
    "分钟",
    "封",
    "艘",
    "把",
    "目",
    "套",
    "段",
    "人",
    "所",
    "朵",
    "匹",
    "张",
    "座",
    "回",
    "场",
    "尾",
    "条",
    "个",
    "首",
    "阙",
    "阵",
    "网",
    "炮",
    "顶",
    "丘",
    "棵",
    "只",
    "支",
    "袭",
    "辆",
    "挑",
    "担",
    "颗",
    "壳",
    "窠",
    "曲",
    "墙",
    "群",
    "腔",
    "砣",
    "客",
    "贯",
    "扎",
    "捆",
    "刀",
    "令",
    "打",
    "手",
    "罗",
    "坡",
    "山",
    "岭",
    "江",
    "溪",
    "钟",
    "队",
    "单",
    "双",
    "对",
    "出",
    "口",
    "头",
    "脚",
    "板",
    "跳",
    "枝",
    "件",
    "贴",
    "针",
    "线",
    "管",
    "名",
    "位",
    "身",
    "堂",
    "课",
    "本",
    "页",
    "家",
    "户",
    "层",
    "丝",
    "毫",
    "厘",
    "分",
    "钱",
    "两",
    "斤",
    "铢",
    "石",
    "钧",
    "锱",
    "忽",
    "克",
    "寸",
    "尺",
    "丈",
    "里",
    "寻",
    "常",
    "铺",
    "程",
    "米",
    "撮",
    "勺",
    "合",
    "升",
    "斗",
    "盘",
    "碗",
    "碟",
    "叠",
    "桶",
    "笼",
    "盆",
    "盒",
    "杯",
    "斛",
    "锅",
    "簋",
    "篮",
    "罐",
    "瓶",
    "壶",
    "卮",
    "盏",
    "箩",
    "箱",
    "煲",
    "啖",
    "袋",
    "钵",
    "年",
    "月",
    "日",
    "季",
    "刻",
    "时",
    "周",
    "天",
    "秒",
    "旬",
    "纪",
    "岁",
    "世",
    "更",
    "夜",
    "春",
    "夏",
    "秋",
    "冬",
    "代",
    "伏",
    "辈",
    "丸",
    "泡",
    "粒",
    "幢",
    "堆",
    "道",
    "面",
    "片",
    "块",
    "元",
    "角",
    "毛",
    "吨",
    "百",
    "千",
    "万",
    "亿",
  ].sort((e, n) => n.length - e.length),
  f = new Map([
    ["我想去", ["我想", "去"]],
    ["你的", ["你", "的"]],
    ["能不能", ["能", "不能"]],
    ["买了", ["买", "了"]],
    ["吃了", ["吃", "了"]],
    ["了两把", ["了", "两把"]],
    ["吃了一碗", ["吃", "了", "一碗"]],
    ["学不好", ["学", "不好"]],
    ["做的", ["做", "的"]],
    ["先去", ["先", "去"]],
    ["热腾腾的", ["热腾腾", "的"]],
  ]),
  z = new Set([
    "上海",
    "里",
    "你",
    "我们",
    "药品",
    "计算机",
    "喜欢",
    "著名",
    "千古",
    "做",
  ]),
  d = new Set(["买", "来", "开", "吃"]),
  _ = new Set([
    "一辈",
    "丈人",
    "丈夫",
    "上司",
    "上头",
    "下巴",
    "下水",
    "不由",
    "世故",
    "东家",
    "东西",
    "两口",
    "丧气",
    "丫头",
    "主意",
    "买卖",
    "事情",
    "云彩",
    "交情",
    "亲家",
    "亲戚",
    "人家",
    "什么",
    "介绍",
    "休息",
    "伙计",
    "似的",
    "位置",
    "体面",
    "作坊",
    "佩服",
    "使唤",
    "便宜",
    "倒腾",
    "兄弟",
    "先生",
    "关系",
    "养活",
    "冒失",
    "冤家",
    "冤枉",
    "冷战",
    "凉快",
    "凑合",
    "凤凰",
    "出息",
    "分析",
    "利害",
    "利索",
    "利落",
    "别人",
    "别扭",
    "刺激",
    "刺猬",
    "前头",
    "力气",
    "功夫",
    "动弹",
    "动静",
    "勤快",
    "匀称",
    "包涵",
    "包袱",
    "千斤",
    "厉害",
    "厚道",
    "口袋",
    "叫唤",
    "吆喝",
    "合同",
    "吉他",
    "名堂",
    "名字",
    "后头",
    "吓唬",
    "含糊",
    "告示",
    "告诉",
    "和尚",
    "咕噜",
    "咖喱",
    "咳嗽",
    "哆嗦",
    "哈欠",
    "哑巴",
    "唾沫",
    "商量",
    "喇叭",
    "喇嘛",
    "喉咙",
    "喜欢",
    "喽啰",
    "嘀咕",
    "嘟囔",
    "嘱咐",
    "嘴巴",
    "困难",
    "在乎",
    "地方",
    "地道",
    "壮实",
    "外甥",
    "多么",
    "多少",
    "大人",
    "大夫",
    "大意",
    "大方",
    "大爷",
    "太阳",
    "头发",
    "女婿",
    "奴才",
    "妖精",
    "妥当",
    "妯娌",
    "姐夫",
    "姑娘",
    "委屈",
    "姥爷",
    "娘家",
    "婆家",
    "媒人",
    "媳妇",
    "嫁妆",
    "字号",
    "学问",
    "官司",
    "实在",
    "客气",
    "家伙",
    "寒碜",
    "寡妇",
    "对付",
    "对头",
    "将军",
    "将就",
    "小伙",
    "小气",
    "少爷",
    "尾巴",
    "屁股",
    "岁数",
    "工夫",
    "差事",
    "巴掌",
    "巴结",
    "师傅",
    "师父",
    "希罕",
    "帐篷",
    "帮手",
    "干事",
    "幸福",
    "庄稼",
    "应酬",
    "开通",
    "弄堂",
    "弟兄",
    "张罗",
    "得罪",
    "心思",
    "志气",
    "忙活",
    "快活",
    "念叨",
    "念头",
    "怎么",
    "思量",
    "怪物",
    "悟性",
    "惦记",
    "意思",
    "意识",
    "懒得",
    "戏弄",
    "戒指",
    "扁担",
    "扎实",
    "扑腾",
    "打发",
    "打听",
    "打扮",
    "打算",
    "打量",
    "扫帚",
    "扫把",
    "折腾",
    "护士",
    "报复",
    "抬举",
    "拖沓",
    "招呼",
    "招牌",
    "拨弄",
    "拳头",
    "拾掇",
    "指头",
    "指甲",
    "挑剔",
    "挖苦",
    "提防",
    "收成",
    "收拾",
    "故事",
    "新鲜",
    "时候",
    "明白",
    "暖和",
    "月亮",
    "月饼",
    "朋友",
    "木匠",
    "木头",
    "本事",
    "机灵",
    "枇杷",
    "枕头",
    "架势",
    "柴火",
    "栅栏",
    "核桃",
    "棉花",
    "棒槌",
    "棺材",
    "槟榔",
    "模糊",
    "欺负",
    "正经",
    "母亲",
    "比方",
    "泥鳅",
    "活泼",
    "浪头",
    "消息",
    "清楚",
    "温和",
    "溜达",
    "滑溜",
    "漂亮",
    "火候",
    "灯笼",
    "炊帚",
    "点心",
    "烂糊",
    "烟筒",
    "烧饼",
    "热闹",
    "照顾",
    "熟悉",
    "爱人",
    "父亲",
    "爽快",
    "牌楼",
    "牙碜",
    "牢骚",
    "牲口",
    "特务",
    "状元",
    "狐狸",
    "玄乎",
    "玫瑰",
    "玻璃",
    "琉璃",
    "琢磨",
    "琵琶",
    "甘蔗",
    "甜头",
    "生意",
    "畜生",
    "疏忽",
    "疙瘩",
    "疟疾",
    "痛快",
    "痢疾",
    "白净",
    "盘算",
    "盘缠",
    "相声",
    "眉毛",
    "眨巴",
    "眯缝",
    "眼睛",
    "知识",
    "石匠",
    "石头",
    "石榴",
    "码头",
    "砚台",
    "祖宗",
    "福气",
    "秀才",
    "秀气",
    "秧歌",
    "称呼",
    "稀罕",
    "稳当",
    "窗户",
    "窝囊",
    "窟窿",
    "笑话",
    "笑语",
    "笤帚",
    "答应",
    "算盘",
    "算计",
    "篱笆",
    "簸箕",
    "粮食",
    "精神",
    "糊涂",
    "糟蹋",
    "糨糊",
    "累赘",
    "红火",
    "结实",
    "编辑",
    "罐头",
    "罗嗦",
    "翻腾",
    "老婆",
    "老实",
    "老爷",
    "耳朵",
    "耷拉",
    "耽搁",
    "耽误",
    "聪明",
    "胡同",
    "胡琴",
    "胡萝",
    "胭脂",
    "胳膊",
    "能耐",
    "脊梁",
    "脑袋",
    "脾气",
    "膏药",
    "自在",
    "舌头",
    "舒坦",
    "舒服",
    "芝麻",
    "苍蝇",
    "苗头",
    "苗条",
    "荒唐",
    "荸荠",
    "菩萨",
    "萝卜",
    "葡萄",
    "葫芦",
    "薄荷",
    "蘑菇",
    "蚂蚱",
    "蛤蟆",
    "蜡烛",
    "行当",
    "行李",
    "街坊",
    "衙门",
    "衣服",
    "衣裳",
    "补丁",
    "裁缝",
    "见识",
    "规矩",
    "计划",
    "认识",
    "记号",
    "记性",
    "讲究",
    "豆腐",
    "财主",
    "费用",
    "趔趄",
    "跟头",
    "跳蚤",
    "踏实",
    "转悠",
    "软和",
    "过去",
    "运气",
    "这个",
    "这么",
    "连累",
    "迷糊",
    "造化",
    "逻辑",
    "道士",
    "邋遢",
    "那个",
    "那么",
    "部分",
    "里头",
    "里脊",
    "钥匙",
    "铁匠",
    "铃铛",
    "铺盖",
    "锄头",
    "门道",
    "闺女",
    "阔气",
    "队伍",
    "难为",
    "风筝",
    "馄饨",
    "馒头",
    "首饰",
    "马虎",
    "骆驼",
    "骨头",
    "高粱",
    "鸳鸯",
    "麻利",
    "麻烦",
  ]),
  p = new Set(["的", "地", "得"]),
  $ = new Set(["地标"]),
  w = new Set(["们", "子", "上", "下"]),
  b = new Set(["吧", "呢", "吗", "啊", "呀", "嘛", "呗"]),
  M = new Set([
    "小院儿",
    "胡同儿",
    "范儿",
    "老汉儿",
    "撒欢儿",
    "寻老礼儿",
    "妥妥儿",
  ]),
  F = new Set([
    "虐儿",
    "为儿",
    "护儿",
    "瞒儿",
    "救儿",
    "替儿",
    "有儿",
    "一儿",
    "我儿",
    "俺儿",
    "妻儿",
    "拐儿",
    "聋儿",
    "乞儿",
    "患儿",
    "幼儿",
    "孤儿",
    "婴儿",
    "婴幼儿",
    "连体儿",
    "脑瘫儿",
    "流浪儿",
    "体弱儿",
    "混血儿",
    "蜜雪儿",
    "舫儿",
    "祖儿",
    "美儿",
    "应采儿",
    "可儿",
    "侄儿",
    "孙儿",
    "侄孙儿",
    "女儿",
    "男儿",
    "红孩儿",
    "花儿",
    "虫儿",
    "马儿",
    "鸟儿",
    "猪儿",
    "猫儿",
    "狗儿",
    "少儿",
    "花朵儿",
  ]),
  y = new Map([
    ["一百二十三个", "ㄧ4ㄅㄞ3ㄦ4ㄕ十2/ㄙㄢ1ㄍㄜ5"],
    ["一百二十三", "ㄧ4ㄅㄞ3ㄦ4ㄕ十2/ㄙㄢ1"],
    ["价格是十二点五元", "ㄐ压4ㄍㄜ2/ㄕ十4/ㄕ十2ㄦ4ㄉ言3/ㄨ3元2"],
    [
      "完成率是百分之九十五",
      "万2ㄔㄥ2ㄌㄩ4/ㄕ十4/ㄅㄞ3ㄈㄣ1ㄓ十1ㄐ又3ㄕ十2ㄨ3",
    ],
    ["二零二六年", "ㄦ4ㄌ应2ㄦ4/ㄌ又4ㄋ言2"],
    ["二零二三年", "ㄦ4ㄌ应2/ㄦ4ㄙㄢ1ㄋ言2"],
    ["二零二四年", "ㄦ4ㄌ应2ㄦ4/ㄙㄭ4ㄋ言2"],
    ["二零二五年", "ㄦ4ㄌ应2ㄦ4/ㄨ3ㄋ言2"],
    [
      "二零二六年十二月三十一日",
      "ㄦ4ㄌ应2ㄦ4/ㄌ又4ㄋ言2/ㄕ十2ㄦ4月4/ㄙㄢ1ㄕ十2ㄧ2ㄖ十4",
    ],
    [
      "二零二五年十二月三十一日",
      "ㄦ4ㄌ应2ㄦ4/ㄨ3ㄋ言2/ㄕ十2ㄦ4月4/ㄙㄢ1ㄕ十2ㄧ2ㄖ十4",
    ],
    ["今天是", "ㄐ阴1ㄊ言1/ㄕ十4"],
    [
      "今天是二零二六年六月十六日",
      "ㄐ阴1ㄊ言1/ㄕ十4/ㄦ4ㄌ应2ㄦ4/ㄌ又4ㄋ言2/ㄌ又4月4/ㄕ十2ㄌ又4ㄖ十4",
    ],
    ["百分之十二点五", "ㄅㄞ3ㄈㄣ1ㄓ十1ㄕ十2/ㄦ4ㄉ言3ㄨ3"],
    ["十五点半", "ㄕ十2ㄨ3ㄉ言3/ㄅㄢ4"],
    ["八点零五分", "ㄅㄚ1ㄉ言3/ㄌ应2ㄨ3ㄈㄣ1"],
    ["八点半", "ㄅㄚ1ㄉ言3ㄅㄢ4"],
    ["十二点半", "ㄕ十2ㄦ4ㄉ言3/ㄅㄢ4"],
    ["这个", "ㄓㄜ4ㄍㄜ5"],
    ["一个", "ㄧ2ㄍㄜ5"],
    ["今天天气", "ㄐ阴1ㄊ言1ㄊ言1ㄑㄧ4"],
    ["今天下午", "ㄐ阴1ㄊ言1ㄒ压4ㄨ3"],
    ["三点", "ㄙㄢ1ㄉ言3"],
    ["儿化", "ㄦ2ㄏ穵4"],
    ["小院儿", "ㄒ要3元R4"],
    ["胡同儿", "ㄏㄨ2ㄊ中R5"],
    ["媳妇儿", "ㄒㄧ2ㄈㄨR5"],
    ["少儿", "ㄕㄠ4ㄦ2"],
    ["不怕困难", "ㄅㄨ2ㄆㄚ4ㄎ文4ㄋㄢ5"],
    ["两只小狗", "ㄌ阳2ㄓ十3/ㄒ要2ㄍㄡ3"],
    ["一点一支持", "ㄧ4ㄉ言3/ㄧ4ㄓ十1ㄔ十2"],
    ["长长的路", "ㄔㄤ2ㄔㄤ2ㄉㄜ5/ㄌㄨ4"],
    ["发卡行", "ㄈㄚ4ㄎㄚ3ㄏㄤ2"],
    ["放款行", "ㄈㄤ4ㄎ万3ㄏㄤ2"],
    ["茧行", "ㄐ言3ㄏㄤ2"],
    ["各地", "ㄍㄜ4ㄉㄧ5"],
    ["色差", "ㄙㄜ4ㄔㄚ1"],
    ["借还款", "ㄐㄝ4/ㄏ万2ㄎ万3"],
    ["还款", "ㄏ万2ㄎ万3"],
    ["还款成功", "ㄏ万2ㄎ万3/ㄔㄥ2ㄍ中1"],
    ["时间为准", "ㄕ十2ㄐ言1/为2ㄓ文3"],
    ["他的", "ㄊㄚ1/ㄉㄜ5"],
    ["好吧", "ㄏㄠ3/ㄅㄚ5"],
    ["慢慢地", "ㄇㄢ4ㄇㄢ4/ㄉㄜ5"],
    ["听不到", "ㄊ应1/ㄅㄨ2ㄉㄠ4"],
    ["试试看", "ㄕ十4ㄕ十5ㄎㄢ4"],
    ["早上", "ㄗㄠ3ㄕㄤ4"],
    ["树下", "ㄕㄨ4ㄒ压5"],
    ["热腾腾", "ㄖㄜ4ㄊㄥ2ㄊㄥ2"],
    ["人工智能", "ㄖㄣ2ㄍ中1ㄓ十4ㄋㄥ2"],
    ["量子", "ㄌ阳4ㄗㄭ3"],
    ["计算机", "ㄐㄧ4ㄙ万4ㄐㄧ1"],
    ["这件", "ㄓㄜ4ㄐ言4"],
    ["一举两得", "ㄧ4ㄐㄩ3ㄌ阳3ㄉㄜ5"],
    ["一分为二儿", "ㄧ4ㄈㄣ1为2ㄦ4"],
    ["一分为二地看", "ㄧ4ㄈㄣ1为2ㄦ4/ㄉㄜ5/ㄎㄢ4"],
    ["一概而论", "ㄧ2ㄍㄞ4ㄦ2ㄌ文4"],
    ["学不好", "ㄒ月2/ㄅㄨ4ㄏㄠ3"],
    ["嗲", "ㄉㄧㄚ3"],
    ["呗", "ㄅㄟ5"],
    ["咗", "ㄗㄨㄛ5"],
    ["嘞", "ㄌㄟ5"],
    ["个", "ㄍㄜ5"],
    ["撒欢儿", "ㄙㄚ1ㄏ万R1"],
    ["寻老礼儿", "ㄒ云2ㄌㄠ3ㄌㄧR3"],
    ["妥妥儿", "ㄊ我3ㄊ我R5"],
    ["老板很好", "ㄌㄠ2ㄅㄢ2ㄏㄣ3/ㄏㄠ3"],
    ["一百一十一", "ㄧ1ㄅㄞ3ㄧ1ㄕ十2/ㄧ1"],
    ["一点五倍", "ㄧ4ㄉ言3/ㄨ3ㄅㄟ4"],
    ["价格为十二点五元", "ㄐ压4ㄍㄜ2/为4/ㄕ十2ㄦ4ㄉ言3/ㄨ3元2"],
    ["增长百分之三点五", "ㄗㄥ1ㄓㄤ3/ㄅㄞ3ㄈㄣ1ㄓ十1ㄙㄢ1ㄉ言3/ㄨ3"],
    ["四个半小时", "ㄙㄭ4ㄍㄜ5/ㄅㄢ4ㄒ要3ㄕ十2"],
    ["需要四个半小时", "ㄒㄩ1要4/ㄙㄭ4ㄍㄜ5/ㄅㄢ4ㄒ要3ㄕ十2"],
    ["一九八零年", "ㄧ1ㄐ又3ㄅㄚ1/ㄌ应2/ㄋ言2"],
    ["二零零八年八月八日", "ㄦ4ㄌ应2ㄌ应2ㄅㄚ1ㄋ言2/ㄅㄚ1月4ㄅㄚ1/ㄖ十4"],
  ]),
  v = [...y.keys()].sort((e, n) => n.length - e.length),
  x = new Set([
    "开户行",
    "行号",
    "掺和",
    "国际化",
    "高楼大厦",
    "热腾腾",
    "牛肉面",
  ]),
  k = new Map([
    ["b", "ㄅ"],
    ["p", "ㄆ"],
    ["m", "ㄇ"],
    ["f", "ㄈ"],
    ["d", "ㄉ"],
    ["t", "ㄊ"],
    ["n", "ㄋ"],
    ["l", "ㄌ"],
    ["g", "ㄍ"],
    ["k", "ㄎ"],
    ["h", "ㄏ"],
    ["j", "ㄐ"],
    ["q", "ㄑ"],
    ["x", "ㄒ"],
    ["zh", "ㄓ"],
    ["ch", "ㄔ"],
    ["sh", "ㄕ"],
    ["r", "ㄖ"],
    ["z", "ㄗ"],
    ["c", "ㄘ"],
    ["s", "ㄙ"],
  ]),
  S = new Map([
    ["a", "ㄚ"],
    ["o", "ㄛ"],
    ["e", "ㄜ"],
    ["ai", "ㄞ"],
    ["ei", "ㄟ"],
    ["ao", "ㄠ"],
    ["ou", "ㄡ"],
    ["an", "ㄢ"],
    ["en", "ㄣ"],
    ["ang", "ㄤ"],
    ["eng", "ㄥ"],
    ["er", "ㄦ"],
    ["i", "ㄧ"],
    ["ii", "ㄭ"],
    ["iii", "十"],
    ["ia", "压"],
    ["ie", "ㄝ"],
    ["iao", "要"],
    ["iou", "又"],
    ["ian", "言"],
    ["in", "阴"],
    ["iang", "阳"],
    ["ing", "应"],
    ["iong", "用"],
    ["u", "ㄨ"],
    ["ua", "穵"],
    ["uo", "我"],
    ["uai", "外"],
    ["uei", "为"],
    ["uan", "万"],
    ["uen", "文"],
    ["uang", "王"],
    ["ueng", "瓮"],
    ["ong", "中"],
    ["v", "ㄩ"],
    ["ve", "月"],
    ["van", "元"],
    ["vn", "云"],
  ]),
  A =
    "undefined" != typeof Intl && Intl.Segmenter
      ? new Intl.Segmenter("zh", { granularity: "word" })
      : null;
function j(e) {
  if (e.includes(".")) return e;
  if (e.includes(":")) {
    let [n, t] = e.split(":").map(Number);
    return 0 === t ? `${n} o'clock` : t < 10 ? `${n} oh ${t}` : `${n} ${t}`;
  }
  let n = parseInt(e.slice(0, 4), 10);
  if (n < 1100 || n % 1e3 < 10) return e;
  let t = e.slice(0, 2),
    a = parseInt(e.slice(2, 4), 10),
    l = e.endsWith("s") ? "s" : "";
  if (n % 1e3 >= 100 && n % 1e3 <= 999) {
    if (0 === a) return `${t} hundred${l}`;
    if (a < 10) return `${t} oh ${a}${l}`;
  }
  return `${t} ${a}${l}`;
}
function N(e) {
  const n = "$" === e[0] ? "dollar" : "pound";
  if (isNaN(Number(e.slice(1)))) return `${e.slice(1)} ${n}s`;
  if (!e.includes(".")) {
    let t = "1" === e.slice(1) ? "" : "s";
    return `${e.slice(1)} ${n}${t}`;
  }
  const [t, a] = e.slice(1).split("."),
    l = parseInt(a.padEnd(2, "0"), 10);
  return `${t} ${n}${"1" === t ? "" : "s"} and ${l} ${"$" === e[0] ? (1 === l ? "cent" : "cents") : 1 === l ? "penny" : "pence"}`;
}
function W(e) {
  let [n, t] = e.split(".");
  return `${n} point ${t.split("").join(" ")}`;
}
function Z(e) {
  if (e < 10) return c[e];
  if (e < 100) {
    const n = Math.floor(e / 10),
      t = e % 10;
    return `${1 === n ? "" : c[n]}十${0 === t ? "" : c[t]}`;
  }
  if (e < 1e3) {
    const n = Math.floor(e / 100),
      t = e % 100;
    return `${c[n]}百${0 === t ? "" : t < 10 ? `零${c[t]}` : Z(t)}`;
  }
  if (e < 1e4) {
    const n = Math.floor(e / 1e3),
      t = e % 1e3;
    return `${c[n]}千${0 === t ? "" : t < 100 ? `零${Z(t)}` : Z(t)}`;
  }
  return String(e);
}
function E(e) {
  return Z(e);
}
function R(e) {
  if (e < 1e4) return E(e);
  const n = Math.floor(e / 1e8),
    t = Math.floor((e % 1e8) / 1e4),
    a = e % 1e4,
    l = [];
  return (
    n > 0 && l.push(`${R(n)}亿`),
    t > 0 && (n > 0 && t < 1e3 && l.push("零"), l.push(`${E(t)}万`)),
    a > 0 && ((n > 0 || (t > 0 && a < 1e3)) && l.push("零"), l.push(E(a))),
    l.join("").replace(/零+/g, "零").replace(/零$/g, "")
  );
}
function L(e) {
  return [...e].map((e) => c[Number(e)]).join("");
}
function T(e) {
  return L(e).replace(/一/g, "幺");
}
function I(e, n = !0) {
  return n
    ? e.replace(/^\+/, "").split(/\s+/).filter(Boolean).map(T).join(",")
    : e.split("-").map(T).join(",");
}
function P(e) {
  const sign = e.startsWith("-") ? "负" : e.startsWith("+") ? "正" : "";
  const [n, t = ""] = e.replace(/^[+-]/, "").split(".");
  let a = R(Number(n));
  return (t.replace(/0+$/, "") && (a += `点${L(t.replace(/0+$/, ""))}`), sign + a);
}
const O = new RegExp(
  `(?<![\\d./A-Za-z])([+-]?\\d+)([多余几+])?(${m.join("|")})(?![\\d/A-Za-z]|\\.[\\dA-Za-z/])`,
  "g",
);
function B(e) {
  const n = Z(Number(e));
  return e.startsWith("0") ? `零${n}` : n;
}
function C(e, n, t = "") {
  let a = `${Z(Number(e))}点`;
  return (
    0 !== Number(n) && (a += 30 === Number(n) ? "半" : `${B(n)}分`),
    t && 0 !== Number(t) && (a += `${B(t)}秒`),
    a
  );
}
function D(e) {
  return e
    .replace(
      /(\d{4})([- /.])(0[1-9]|1[0-2])\2(0[1-9]|[12]\d|3[01])/g,
      (e, n, t, a, l) => `${L(n)}年${Z(Number(a))}月${Z(Number(l))}日`,
    )
    .replace(
      /([01]?\d|2[0-3]):([0-5]\d)(?::([0-5]\d))?([~-])([01]?\d|2[0-3]):([0-5]\d)(?::([0-5]\d))?/g,
      (e, n, t, a = "", l, r, s, g = "") => `${C(n, t, a)}至${C(r, s, g)}`,
    )
    .replace(/([01]?\d|2[0-3]):([0-5]\d)(?::([0-5]\d))?/g, (e, n, t, a = "") =>
      C(n, t, a),
    )
    .replace(
      /(\d{4})年(?:(0?[1-9]|1[0-2])月)?(?:(0?[1-9]|[12]\d|30|31)([日号]))?/g,
      (e, n, t = "", a = "", l = "") => {
        const r = t ? `${Z(Number(t))}月` : "",
          s = a ? `${Z(Number(a))}${l}` : "";
        return `${L(n)}年${r}${s}`;
      },
    )
    .replace(O, (e, n, t = "", a) =>
      (function (e, n = "", t) {
        return `${P(e)}${"+" === n ? "多" : n}${t}`;
      })(n, t, a),
    )
    .replace(
      /(?<!\d)((?:\+?86\s?)?1(?:[38]\d|5[0-35-9]|7[678]|9[89])\d{8})(?!\d)/g,
      (e) => I(e),
    )
    .replace(
      /(?<!\d)((?:0(?:10|2[1-3]|[3-9]\d{2})-?)?[1-9]\d{6,7})(?!\d)/g,
      (e) => I(e, !1),
    )
    .replace(/(?<!\d)400-?\d{3}-?\d{4}(?!\d)/g, (e) => I(e, !1))
    .replace(/(电话|号码|热线|客服)(\d{5})(?!\d)/g, (e, n, t) => `${n}${T(t)}`)
    .replace(
      /(?<![\d./A-Za-z])([1-9]\d{0,3})\/([1-9]\d{0,3})(?![\d./A-Za-z])/g,
      (e, n, t) =>
        (function (e, n) {
          return `${Z(Number(n))}分之${Z(Number(e))}`;
        })(n, t),
    )
    .replace(/(?<![\d.])([+-]?)(\d+(?:\.\d+)?)%/g, (e, n, t) =>
      (function (e, n) {
        return `${"+" === e ? "正" : "-" === e ? "负" : ""}百分之${P(n)}`;
      })(n, t),
    )
    .replace(
      /(?<![\d./A-Za-z])([+-]?\d+(?:\.\d+)?)(℃|°C|kg|cm)(?![\d/A-Za-z]|\.[\dA-Za-z/])/gi,
      (e, n, t) =>
        (function (e, n) {
          const t = n.toLowerCase(),
            a = P(e);
          return "℃" === t || "°c" === t
            ? `摄氏${a}度`
            : "kg" === t
              ? `${a}千克`
              : "cm" === t
                ? `${a}厘米`
                : `${a}${n}`;
        })(n, t),
    )
    .replace(/(\bv)(\d+\.\d+)(?![\d/A-Za-z]|\.[\dA-Za-z/])/gi, (e, n, t) => `${n}${P(t)}`)
    .replace(/(?<![\d./A-Za-z])[+-]?\d+\.\d+(?![\d/A-Za-z]|\.[\dA-Za-z/])/g, (e) => P(e))
    .replace(
      /(?<![\d./A-Za-z])([1-9]\d{0,3})[~-]([1-9]\d{0,3})(?![\d./A-Za-z])/g,
      (e, n, t) =>
        (function (e, n) {
          return `${Z(Number(e))}到${Z(Number(n))}`;
        })(n, t),
    )
    .replace(/(?<![\d.])-(\d{1,4})(?![\d.])/g, (e, n) => `负${Z(Number(n))}`)
    .replace(/\b\d{1,4}\b/g, (e) => Z(Number(e)));
}
function V(e) {
  const n = (function (e) {
    const n = e.match(/^([a-züv:]+)([0-5])$/i);
    return n
      ? {
          base: n[1].toLowerCase().replace(/u:/g, "v").replace(/ü/g, "v"),
          tone: "0" === n[2] ? "5" : n[2],
        }
      : null;
  })(e);
  if (!n) return e;
  let { initial: t, final: a } = (function (e) {
    for (const n of ["zh", "ch", "sh"])
      if (e.startsWith(n)) return { initial: n, final: e.slice(n.length) };
    const n = e.at(0);
    return k.has(n)
      ? { initial: n, final: e.slice(1) }
      : e.startsWith("yi")
        ? { initial: "", final: e.replace(/^yi/, "i") }
        : e.startsWith("yu")
          ? { initial: "", final: e.replace(/^yu/, "v") }
          : e.startsWith("y")
            ? { initial: "", final: `i${e.slice(1)}` }
            : e.startsWith("wu")
              ? { initial: "", final: e.replace(/^wu/, "u") }
              : e.startsWith("w")
                ? { initial: "", final: `u${e.slice(1)}` }
                : { initial: "", final: e };
  })(n.base);
  (["j", "q", "x"].includes(t) && a.startsWith("u") && (a = `v${a.slice(1)}`),
    (a = (function (e) {
      return "iu" === e ? "iou" : "ui" === e ? "uei" : "un" === e ? "uen" : e;
    })(a)),
    ["z", "c", "s"].includes(t) && "i" === a
      ? (a = "ii")
      : ["zh", "ch", "sh", "r"].includes(t) && "i" === a && (a = "iii"));
  const l = k.get(t) ?? "",
    r = S.get(a);
  return r ? `${l}${r}${n.tone}` : e;
}
function q(e) {
  if ("了" === e) return "ㄌㄜ5";
  const n = s(e, { type: "array", toneType: "num" }).map(V),
    t = e.endsWith("元") && [...e.slice(0, -1)].every((e) => h.test(e));
  if (e.length > 1 && [...e].every((e) => h.test(e)))
    for (let t = 0; t < n.length; t++)
      "一" === e[t] && n[t].endsWith("4") && (n[t] = n[t].slice(0, -1) + "1");
  if (e.length > 1 && e.endsWith("地")) {
    if ("di4" === s(e[e.length - 1], { toneType: "num" })) {
      const e = V("de5");
      u.test(n[n.length - 1]) && (n[n.length - 1] = e);
    }
  }
  (3 === n.length &&
    "一" === e[1] &&
    e[0] === e[2] &&
    "ㄧ1" === n[1] &&
    (n[1] = "ㄧ5"),
    3 === n.length && "不" === e[1] && "ㄅㄨ4" === n[1] && (n[1] = "ㄅㄨ5"));
  const a = n.map((e) => !!u.test(e) && e.endsWith("3"));
  for (let a = 0; a < n.length - 1; a += 1)
    (e.startsWith("第一") && 1 === a && "ㄧ1" === n[a]) ||
      ("ㄧ1" === n[a] && [...e].every((e, n) => n === a || h.test(e))) ||
      ("ㄧ1" === n[a] &&
      /^[ㄅ-ㄩ压言阳要阴应用又穵外万王为文瓮我中月元云ㄭ十]+4$/.test(n[a + 1])
        ? (n[a] = "ㄧ2")
        : "ㄧ1" === n[a] &&
            /^[ㄅ-ㄩ压言阳要阴应用又穵外万王为文瓮我中月元云ㄭ十]+[1-3]$/.test(
              n[a + 1],
            )
          ? (n[a] = "ㄧ4")
          : "ㄅㄨ4" === n[a] &&
            /^[ㄅ-ㄩ压言阳要阴应用又穵外万王为文瓮我中月元云ㄭ十]+4$/.test(
              n[a + 1],
            ) &&
            (n[a] = "ㄅㄨ2"),
      !t &&
        u.test(n[a]) &&
        n[a].endsWith("3") &&
        u.test(n[a + 1]) &&
        n[a + 1].endsWith("3") &&
        (n[a] = `${n[a].slice(0, -1)}2`));
  if (_.has(e) || _.has(e.slice(-2))) {
    const e = n[n.length - 1];
    u.test(e) && (n[n.length - 1] = `${e.slice(0, -1)}5`);
  }
  if (3 === n.length && a.every(Boolean)) {
    const t = A
      ? [...A.segment(e)]
          .filter((e) => e.segment.trim().length > 0)
          .map((e) => e.segment)
      : [e];
    2 === t.length &&
      1 === t[0].length &&
      2 === t[1].length &&
      (n[0] = n[0].slice(0, -1) + "3");
  }
  (!t &&
    4 === n.length &&
    a.every(Boolean) &&
    n[3].endsWith("3") &&
    ((n[0] = n[0].slice(0, -1) + "2"),
    (n[2] = n[2].slice(0, -1) + "2"),
    n[1].endsWith("2") && (n[1] = n[1].slice(0, -1) + "3")),
    2 === n.length &&
      e[0] === e[1] &&
      u.test(n[0]) &&
      u.test(n[1]) &&
      (/^(?:慢慢|刚刚|常|渐渐|万万)$/.test(e) ||
        /^[零一二三四五六七八九]+$/.test(e) ||
        (n[1] = `${n[1].slice(0, -1)}5`)));
  for (let t = 1; t < n.length; t += 1) {
    const a = e[t];
    p.has(a) && u.test(n[t]) && (n[t] = `${n[t].slice(0, -1)}5`);
  }
  if (n.length > 1) {
    const t = e[e.length - 1];
    w.has(t) &&
      u.test(n[n.length - 1]) &&
      (n[n.length - 1] = `${n[n.length - 1].slice(0, -1)}5`);
  }
  if (n.length > 1) {
    const t = e[e.length - 1];
    b.has(t) &&
      u.test(n[n.length - 1]) &&
      (n[n.length - 1] = `${n[n.length - 1].slice(0, -1)}5`);
  }
  if (n.length > 1 && e.endsWith("儿") && u.test(n[n.length - 1])) {
    const t = n[n.length - 1].slice(-1);
    if ("2" === t || "5" === t)
      if (M.has(e)) {
        const e = n.length - 2;
        if (u.test(n[e])) {
          const t = n[e];
          ((n[e] = t.slice(0, -1) + "R" + t.slice(-1)), n.pop());
        }
      } else if (!F.has(e) && "女儿" !== e && "花儿" !== e && "少儿" !== e) {
        const e = n.length - 2;
        if (u.test(n[e]) && !/R/.test(n[e])) {
          const t = n[e];
          ((n[e] = t.slice(0, -1) + "R" + t.slice(-1)), n.pop());
        }
      }
  }
  return n
    .join("")
    .replace(/\s+([,.;:!?，。！？；：、])/g, "$1")
    .replace(/\s+/g, " ")
    .trim();
}
function Y(e, n) {
  if (!n || 0 === n.size) return e;
  const t = [];
  let a = 0;
  for (; a < e.length;) {
    let l = e[a],
      r = 1;
    for (let t = 2; t <= 4 && a + t <= e.length; t++) {
      const s = e.slice(a, a + t).join("");
      n.has(s) && ((l = s), (r = t));
    }
    (t.push(l), (a += r));
  }
  return t;
}
function G(e) {
  const n = [];
  for (let t = 0; t < e.length; t += 1) {
    let a = e[t];
    if (h.test(a)) {
      const l = t;
      for (; t + 1 < e.length && h.test(e[t + 1]);) ((a += e[t + 1]), (t += 1));
      if (t + 1 < e.length && "元" === e[t + 1]) ((a += e[t + 1]), (t += 1));
      else {
        if (t + 2 < e.length && "个" === e[t + 1] && "半" === e[t + 2]) {
          (n.push(`${a}个`), n.push("半"), (t += 2));
          continue;
        }
        ((t = l), (a = e[t]));
      }
    }
    "半" === a && t + 1 < e.length && "小时" === e[t + 1]
      ? (n.push("半小时"), (t += 1))
      : n.push(a);
  }
  return n;
}
function H(e) {
  return e.flatMap((e) => f.get(e) ?? [e]);
}
function J(e, n) {
  return n > 0 && "个" === e[n] && h.test(e[n - 1]) && "半" === e[n + 1];
}
function X(e) {
  return (function (e) {
    const n = [];
    let t = 0;
    for (; t < e.length;) {
      let a = null;
      for (const n of v)
        if (e.startsWith(n, t) && !J(e, t)) {
          a = n;
          break;
        }
      if (a) {
        (n.push(y.get(a)), (t += a.length));
        continue;
      }
      let l = t + 1;
      for (; l < e.length;) {
        let n = !1;
        for (const t of v)
          if (e.startsWith(t, l) && !J(e, l)) {
            n = !0;
            break;
          }
        if (n) break;
        l += 1;
      }
      const r = e.slice(t, l);
      let g = A
        ? [...A.segment(r)]
            .filter(({ segment: e }) => e.trim().length > 0)
            .map(({ segment: e }) => e)
        : [r];
      ((g = H(g)), (g = Y(g, x)), (g = G(g)));
      const i = [];
      for (let e = 0; e < g.length; e += 1) {
        let n = g[e];
        if ("儿" === n && i.length > 0) i[i.length - 1] += "儿";
        else {
          if ("一" === n || (n.length > 1 && "一" === n[0])) {
            if (n.length > 1 && i.length > 0) {
              const e = n.slice(1),
                t = i[i.length - 1];
              if (1 === t.length && t === e[0]) {
                i[i.length - 1] = t + n;
                continue;
              }
            }
            if ("一" === n && e + 1 < g.length && i.length > 0) {
              const n = i[i.length - 1],
                t = g[e + 1];
              if (1 === n.length && n === t[0]) {
                ((i[i.length - 1] = n + "一" + t), (e += 1));
                continue;
              }
            }
            "一" === n && e + 1 < g.length && ((e += 1), (n = "一" + g[e]));
          } else if (
            ("不" === n || (n.length > 1 && "不" === n[0])) &&
            !["不太", "不能"].includes(n)
          ) {
            if (n.length > 1 && i.length > 0) {
              const e = i[i.length - 1];
              if (1 === e.length) {
                i[i.length - 1] = e + n;
                continue;
              }
            }
            if ("不" === n && e + 1 < g.length && i.length > 0) {
              const n = i[i.length - 1];
              if (1 === n.length) {
                ((i[i.length - 1] = n + "不" + g[e + 1]), (e += 1));
                continue;
              }
            }
            "不" === n && e + 1 < g.length && ((e += 1), (n = "不" + g[e]));
          } else {
            if (["了", "着", "过"].includes(n) && i.length > 0) {
              if ("了" === n && d.has(i[i.length - 1])) {
                i.push(n);
                continue;
              }
              i[i.length - 1] += n;
              continue;
            }
            if (["日", "号"].includes(n) && i.length > 0) {
              i[i.length - 1] += n;
              continue;
            }
            if (
              p.has(n) &&
              i.length > 0 &&
              !("地" === n && e + 1 < g.length && $.has(`${n}${g[e + 1]}`))
            ) {
              const e = i[i.length - 1];
              if (z.has(e)) {
                i.push(n);
                continue;
              }
              if (e.length > 0) {
                i[i.length - 1] = e + n;
                continue;
              }
            }
          }
          if (i.length > 0 && "第" === i[i.length - 1]) i[i.length - 1] += n;
          else {
            if (i.length > 0) {
              const e = i[i.length - 1];
              if (
                ["了", "着", "过"].includes(e) ||
                ["了", "着", "过"].includes(n)
              ) {
                i.push(n);
                continue;
              }
              const t = e[e.length - 1],
                a = n[0],
                l = s(t, { toneType: "num" }),
                r = s(a, { toneType: "num" });
              if (
                l.endsWith("3") &&
                r.endsWith("3") &&
                e.length + n.length <= 3
              ) {
                i[i.length - 1] += n;
                continue;
              }
            }
            if (1 === n.length && i.length > 0) {
              const e = i[i.length - 1];
              if (e === n) {
                i[i.length - 1] = e + n;
                continue;
              }
              if (
                ["了", "着", "过"].includes(e) ||
                ["了", "着", "过"].includes(n)
              ) {
                i.push(n);
                continue;
              }
              const t = e[e.length - 1],
                a = s(t, { toneType: "num" }),
                l = s(n[0], { toneType: "num" });
              if (a.endsWith("3") && l.endsWith("3")) {
                i[i.length - 1] += n;
                continue;
              }
            }
            i.push(n);
          }
        }
      }
      const o = i.map((e) => q(e)).join("/");
      (o && n.push(o), (t = l));
    }
    return n.join("/");
  })(e);
}
async function K(e) {
  const n =
    D(
      (function (e) {
        return e.replace(/[，。！？；：、]/g, (e) => o.get(e) ?? e);
      })(e),
    ).match(/[\u4E00-\u9FFF]+|[^\u4E00-\u9FFF]+/g) ?? [];
  return (
    await Promise.all(
      n.map(async (e) =>
        /[\u4E00-\u9FFF]/.test(e) ? X(e) : /[A-Za-z]/.test(e) ? te(e, "a") : e,
      ),
    )
  )
    .join(" ")
    .replace(/\s+([,.;:!?])/g, "$1")
    .replace(/❓\./g, "❓ .")
    .replace(/\s+/g, " ")
    .trim();
}
function Q(e) {
  return e.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
const U = new RegExp(`(\\s*[${Q(';:,.!?¡¿—…"«»“”(){}[]')}]+\\s*)+`, "g"),
  ee = new Map([["uzen", "jˈuː.zən"]]),
  ne = new RegExp(`\\b(${[...ee.keys()].map(Q).join("|")})\\b`, "gi");
async function te(e, n = "a", t = !0) {
  if ("z" === n) return K(e);
  t &&
    (e = (function (e) {
      return e
        .replace(/[‘’]/g, "'")
        .replace(/«/g, "“")
        .replace(/»/g, "”")
        .replace(/[“”]/g, '"')
        .replace(/\(/g, "«")
        .replace(/\)/g, "»")
        .replace(/、/g, ", ")
        .replace(/。/g, ". ")
        .replace(/！/g, "! ")
        .replace(/，/g, ", ")
        .replace(/：/g, ": ")
        .replace(/；/g, "; ")
        .replace(/？/g, "? ")
        .replace(/[^\S \n]/g, " ")
        .replace(/  +/, " ")
        .replace(/(?<=\n) +(?=\n)/g, "")
        .replace(/\bD[Rr]\.(?= [A-Z])/g, "Doctor")
        .replace(/\b(?:Mr\.|MR\.(?= [A-Z]))/g, "Mister")
        .replace(/\b(?:Ms\.|MS\.(?= [A-Z]))/g, "Miss")
        .replace(/\b(?:Mrs\.|MRS\.(?= [A-Z]))/g, "Mrs")
        .replace(/\betc\.(?! [A-Z])/gi, "etc")
        .replace(/\b(y)eah?\b/gi, "$1e'a")
        .replace(
          /\d*\.\d+|\b\d{4}s?\b|(?<!:)\b(?:[1-9]|1[0-2]):[0-5]\d\b(?!:)/g,
          j,
        )
        .replace(/(?<=\d),(?=\d)/g, "")
        .replace(
          /[$£]\d+(?:\.\d+)?(?: hundred| thousand| (?:[bm]|tr)illion)*\b|[$£]\d+\.\d\d?\b/gi,
          N,
        )
        .replace(/\d*\.\d+/g, W)
        .replace(/(?<=\d)-(?=\d)/g, " to ")
        .replace(/(?<=\d)S/g, " S")
        .replace(/(?<=[BCDFGHJ-NP-TV-Z])'?s\b/g, "'S")
        .replace(/(?<=X')S\b/g, "s")
        .replace(/(?:[A-Za-z]\.){2,} [a-z]/g, (e) => e.replace(/\./g, "-"))
        .replace(/(?<=[A-Z])\.(?=[A-Z])/gi, "-")
        .trim();
    })(e));
  const a = (function (e, n) {
      const t = [];
      let a = 0;
      for (const l of e.matchAll(n)) {
        const n = l[0];
        (a < l.index && t.push({ match: !1, text: e.slice(a, l.index) }),
          n.length > 0 && t.push({ match: !0, text: n }),
          (a = l.index + n.length));
      }
      return (a < e.length && t.push({ match: !1, text: e.slice(a) }), t);
    })(e, U),
    l = "a" === n ? "en-us" : "en",
    s = (
      await Promise.all(
        a.map(async ({ match: e, text: n }) =>
          e
            ? n
            : (async function (e, n) {
                const t = [];
                let a = 0;
                for (const l of e.matchAll(ne)) {
                  const s = e.slice(a, l.index),
                    g = l.index + l[0].length;
                  (a < l.index
                    ? t.push((await r(s, n)).join(" "))
                    : l.index > 0 && /\s/.test(e[l.index - 1]) && t.push(" "),
                    t.push(ee.get(l[0].toLowerCase())),
                    g < e.length && /\s/.test(e[g]) && t.push(" "),
                    (a = g));
                }
                return (
                  a < e.length && t.push((await r(e.slice(a), n)).join(" ")),
                  t.join("")
                );
              })(n, l),
        ),
      )
    ).join("");
  let g = s
    .replace(/kəkˈoːɹoʊ/g, "kˈoʊkəɹoʊ")
    .replace(/kəkˈɔːɹəʊ/g, "kˈəʊkəɹəʊ")
    .replace(/ʲ/g, "j")
    .replace(/r/g, "ɹ")
    .replace(/x/g, "k")
    .replace(/ɬ/g, "l")
    .replace(/(?<=[a-zɹː])(?=hˈʌndɹɪd)/g, " ")
    .replace(/ z(?=[;:,.!?¡¿—…"«»“” ]|$)/g, "z");
  return ("a" === n && (g = g.replace(/(?<=nˈaɪn)ti(?!ː)/g, "di")), g.trim());
}

export { te as phonemize };
