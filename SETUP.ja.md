# めも（使い方）

ガジェットに物理的なボタンを追加。
特に表示しかできない、Disp機に有効。
ガジェット側は、どのボタンを押したかを送るだけ、受け側の Muse が押されたボタンがわかるので、それに合わせたアクション（スキル）などを仕込んでおけば、リモコンなどに利用できる。

## 有効にするには

使用するボードの sdkconfig に

```
CONFIG_HOMEHUB_SIGNAL_BUTTON
CONFIG_HOMEHUB_SIGNAL_GPIO
```

を設定する。

例:

```
CONFIG_HOMEHUB_SIGNAL_BUTTON=y
CONFIG_HOMEHUB_SIGNAL_GPIO=7
```

ボタン設定は、最高30個を設定できるが、ガジェット起動時にボタン設定が読み込まれるため、sdkconfig では一つで構わない。

## 仕込みについて

＃重要 事前に Muse にルールを覚えさせる。

そのルール（ある意味 Muse の setup）は、めもとは分けて [muse_setup.md](muse_setup.md) に置いてある。そのまま自分の Muse に読ませて、割り当てとアクションを自分の内容に書き換えてもらう。Disp機のアクション実例（4分割のボタン用画像、押したら実行画面、5秒で戻る）もそちらに入れてある。

ユーザが書く割り当ては、短い形でよい。

```
BTN1=18
BTN2=23
BTN3=24
```

CFGREQ への返事で CSV 形式に清書するのは、Muse 側の仕事。
