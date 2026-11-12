# termux-ghost

[English](README.md) | **العربية**

[![CI](https://github.com/stonxmg-lang/termux-ghost/actions/workflows/ci.yml/badge.svg)](https://github.com/stonxmg-lang/termux-ghost/actions/workflows/ci.yml)

اقتراحات تلقائية من سجل الأوامر لصدفة Bash على ترمكس، تعمل مثل `zsh-autosuggestions`.
مكتوبة بلغة C وتعمل داخل Bash مباشرة عبر مكتبة readline، بلا عمليات خارجية عند كل حرف.

![عرض termux-ghost: يظهر اقتراح باهت من السجل أثناء الكتابة، والسهم → يقبله](docs/demo.gif)

## التثبيت

    pkg install git
    git clone https://github.com/stonxmg-lang/termux-ghost
    cd termux-ghost
    ./install.sh

ثم افتح جلسة جديدة. يبني السكربت الإضافة على جهازك، ويثبّت `clang` و`make` و`readline` إن لم تكن موجودة.

الإزالة: `./install.sh --uninstall`

## الاستخدام

| المفتاح | الوظيفة |
|---|---|
| `→` أو `End` أو `Ctrl-E` أو `Ctrl-F` | قبول الاقتراح كاملًا |
| `Alt + →` أو `Ctrl + →` أو `Alt-F` | قبول الكلمة التالية فقط |

| الأمر | الوظيفة |
|---|---|
| `ghost on` / `ghost off` | تشغيل أو إيقاف |
| `ghost status` | عرض الحالة |
| `ghost color 38;5;244` | تغيير لون الاقتراح (رموز SGR) |
| `ghost ignore` | عرض أنماط التجاهل |
| `ghost ignore '*نمط*'` | عدم اقتراح أي سطر من السجل يطابق النمط |
| `ghost unignore '*نمط*'` | حذف نمط |
| `ghost ignore --clear` / `--reset` | حذف كل الأنماط / إرجاع الافتراضية |

الأوامر تسري على الجلسة الحالية فقط. لتثبيت لون، أضف أمر `ghost color` إلى `~/.bashrc` بعد سطر التحميل.

الاقتراحات تأتي من السجل المحمّل في الجلسة، وحجمه يحدده `HISTSIZE` و`HISTFILESIZE`.

## إخفاء الأسرار

إذا كتبت مرة كلمة مرور أو توكن في سطر الأوامر، فلن ترغب في ظهورها على الشاشة من جديد. أسطر السجل المطابقة لنمط تجاهل لا تُقترح أبدًا، ويظهر بدلًا منها أحدث سطر آمن.

هذه الأنماط مفعّلة افتراضيًا، ولا تفرّق بين الأحرف الكبيرة والصغيرة:

```
*password*  *passwd*  *secret*  *token*  *api_key*  *apikey*
*authorization:*  *ghp_*  *github_pat_*
```

أضف أنماطك بـ `ghost ignore`. **ضع النمط بين علامتي اقتباس**، وإلا قد يحوّل Bash علامة `*` إلى أسماء ملفات:

```sh
ghost ignore '*mysql -p*' '*aws_secret*'
```

`*` تطابق أي شيء، و`?` تطابق حرفًا واحدًا. مثل باقي الأوامر، يسري هذا على الجلسة الحالية فقط، فأضفه إلى `~/.bashrc` بعد سطر التحميل لتثبيته.

هذا يخفي الاقتراحات فقط، والأوامر تبقى في `~/.bash_history`. لمنع حفظ أمر في السجل أصلًا، اضبط `HISTCONTROL=ignorespace` وابدأ الأمر بمسافة.

## التوافق

تعمل مع Bash فقط، وليس مع Zsh أو Fish.

جُرّبت على: ترمكس، Android 16، aarch64، Bash 5.3، readline 8.3. لم تُجرَّب بعد على إصدارات أو معماريات أخرى، لكنها تُبنى على جهازك نفسه. إذا تعذّر تحميلها تعمل الصدفة عادي دون اقتراحات.

## الاختبارات

    tests/run.sh

تعمل في طرفية حقيقية عبر tmux، بسجل مؤقت لا يمس سجلك.

تحتاج `tmux`.

تشغّلها GitHub Actions مع كل push داخل ترمكس حقيقي ([termux-docker](https://github.com/termux/termux-docker))، على x86_64 وعلى aarch64 عبر QEMU، ومعها اختبارات وحدة تحت AddressSanitizer (`tests/unit.c`). وتعمل أيضًا كل أسبوع، فإذا كسر تحديث لترمكس شيئًا يظهر ذلك بسرعة.

## الرخصة

MIT
