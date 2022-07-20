# Communication txproto-KYMUX

Ce document a pour but de clarifier les difficultés que je rencontre pour faire
communiquer _txproto_ avec un muxer (que l'on nommera KYMUX) destiné à être
exécuté dans un processus séparé.

La première partie ("Architecture") présente le contexte nécessaire et les
objectifs. La seconde partie ("Difficultés") décrit les problèmes rencontrés,
les solutions envisagées et investiguées, et en quoi elles ne sont pas
satisfaisantes d'après moi. La dernière partie ("Besoins") présente ce dont on
aurait besoin pour simplifier le problème selon moi (d'autres idées sont
bienvenues), et pourra servir à des discussions futures.


## Architecture

La suite de la discussion suppose l'architecture suivante :

```
             SERVER                                        CLIENT
<------------------------------->             <------------------------------->

                      +-------+                 +-------+
   video capture ---> |       | QUIC connection |       | ---> VLC access/demux
   audio capture ---> | KYMUX |=================| KYMUX | <--- input forwarding
 input injection <--- |       |                 |       | IPC
                  IPC +-------+                 +-------+
```

Chaque composant à gauche (la capture video, la capture audio, etc.) est exécuté
dans un processus séparé (démarré par un _controller_, non représenté sur le
schéma).

Côté serveur, KYMUX communique par IPC (voir ci-dessous) avec chacun de ces
processus pour recevoir les flux vidéo et audio à transmettre au client,
transmettre les _inputs_ provenant des clients, etc.

Le but de KYMUX est de gérer la communication entre le client et le serveur
(dans les 2 sens) sur une unique connexion QUIC. Il est responsable de la bonne
gestion de la perte de paquets selon le flux, la correction d'erreurs, etc. Il
pourra par exemple suivre le protocole [Qproto], ou tout autre protocole
adéquat.

[Qproto]: https://github.com/cyanreg/qproto/blob/master/draft-qproto-spec.md


### IPC

Étant donné que la capture et l'injection d'_inputs_ sont effectués dans des
processus séparés, KYMUX doit communiquer avec ces processus, par exemple via
un pipe ou une socket. Pour que ça fonctionne simplement sur toutes les
plates-formes, pour commencer nous allons utiliser une simple socket TCP
locale.

Cette communication permettra en particulier à KYMUX de recevoir en entrée les
flux vidéo et audio qu'il devra transmettre au client sur la connexion QUIC.

Le protocole doit être le plus simple possible pour KYMUX, qui veut juste avoir
accès aux paquets d'entrée, dans l'ordre, avec les meta-données nécessaires (le
PTS, la taille du paquet pour éviter 1 frame de latence, quelques flags…), rien
de plus. En particulier, nous ne voulons surtout pas communiquer via un
protocole tel que RTP entre les processus de capture et KYMUX.

En guise d'illustration, voici ce à quoi pourrait ressembler le protocole pour
la vidéo (c'est ce qui est utilisé dans [scrcpy] et dans le premier [PoC
0latency]) :

[scrcpy]: https://github.com/Genymobile/scrcpy/blob/db8c1ce8e1e4a7d3f96fad7d36a281798f529453/app/src/demuxer.c#L22-L41
[PoC 0latency]: https://code.videolan.org/rom1v/vlc/-/merge_requests/20

```c
    // The video stream contains raw packets, without time information. When we
    // record, we retrieve the timestamps separately, from a "meta" header
    // added by the server before each raw packet.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.
    //
    // The most significant bits of the PTS are used for packet flags:
    //
    //  byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1   byte 0
    // CK...... ........ ........ ........ ........ ........ ........ ........
    // ^^<------------------------------------------------------------------->
    // ||                                PTS
    // | `- key frame
    //  `-- config packet
```

En pratique, les SPS/PPS sont envoyés en premier (en tant que _config packet_),
suivi par le flux vidéo. Dans KYMUX, les SPS/PPS seront sûrement envoyés sur un
stream QUIC _reliable_ et le flux vidéo dans des paquets QUIC _unreliable_.

Ce n'est qu'un exemple, nous pourrons adapter le format à notre guise. Nous
devrons également définir un format pour l'audio et les _inputs_.


### txproto

Pour capturer l'écran et le son sur les différentes plates-formes, nous
utilisons [txproto]. Concrètement, nous écrivons un script LUA pour
configurer les entrées (ce qui doit être capturé), le format d'encodage et les
sorties.

[txproto]: https://github.com/cyanreg/txproto

Dans notre PoC actuel, _txproto_ envoie les flux vidéo et audio sur
RTP vers l'IP du client. Nous le configurons dans notre script LUA, et
_txproto_ transmet ces informations à FFmpeg qui effectue le _mux_ (ici, la
transmission sur RTP) :


```lua
    muxer = tx.create_muxer({
        out_url = string.format("rtp://%s:%s", rtp_host, rtp_port),
        out_format = "rtp",
        options = {
            max_interleave_delta = 0,
        },
        priv_options = priv_options,
    })
    muxer.link(encoder)
```


## Difficultés

Pour notre architecture cible, le processus qui effectue la capture (vidéo
et/ou audio) n'est pas le même que celui qui effectue le mux (KYMUX).

Cependant, _txproto_ gère nécessairement à la fois la capture et le mux (depuis
le même processus).

Pour tenter de contourner ce problème, nous pouvons essayer d'utiliser comme
format de sortie du mux de _txproto_ notre protocole _custom_ (par exemple
`flags|PTS|size|raw_packet` comme décrit plus haut), plutôt que le protocole
utilisé entre le client et le serveur (qui sera géré par KYMUX).

Le langage LUA de txproto permet uniquement de configurer les entrées, les
sorties et les options, pas de créer un nouveau muxer. Créer un protocole
_custom_ implique donc que modifier le code.


### Tentative 1 : hacker txproto

Une approche possible est de forker _txproto_ et de modifier le code de
[`muxing.c`] pour gérer un protocole supplémentaire (`"kyber"`) de manière
spécifique.

[`muxing.c`]: https://github.com/cyanreg/txproto/blob/1e3da0b55d9694a9998eecfb3cee83faf7b10ca0/src/muxing.c

Cependant, _txproto_ est cablé (hardcodé) pour déléguer le mux à FFmpeg. Il n'y
a pas de gestion de _modules_ ni d'abstraction pour fournir une implémentation
alternative (ce qui est cohérent, il est fait pour fonctionner avec FFmpeg et
n'a pas vocation à gérer lui-même ces problématiques).

Bidouiller _txproto_ pour y ajouter la gestion de notre protocole _custom_ est
donc très intrusif, et provoquera des conflits à chaque modifications (et nos
protocoles _custom_ n'ont pas à être mergés _upstream_). De plus, cela semble
aller à l'encontre du design de _txproto_.


### Tentative 2 : ajouter un muxer FFmpeg

Vu que _txproto_ délègue le mux à FFmpeg, nous pouvons ajouter un module de mux
à FFmpeg pour produire notre format _custom_.

J'ai écrit un PoC qui écrit un header contenant le PTS et la taille du paquet,
suivi du paquet lui-même : [`libavformat/kyber.c`][kyber.c].

[kyber.c]: https://github.com/rom1v/FFmpeg/commit/6e9c4a3045855ead397214c5b6599b1ec8c29d93

_La gestion des SPS/PPS n'est pas implémentée, et contrairement à l'exemple
présenté plus haut, il n'y a pas de flags "control packet" ni "keyframe"._

Après avoir compilé FFmpeg avec ce module, on peut le tester. Tout d'abord,
écouter sur une socket TCP et afficher le contenu lu en hexa :

```bash
nc -l -p 1234 | hd | less
```

(`nc` est l'équivalent de `netcat`, et `hd` est l'équivalent de `hexdump -C`)

Puis démarrer une capture avec FFmpeg en utilisant `x11grab`, le format `kyber`
(pour activer notre module) et une sortie sur `tcp://localhost:1234`:

```bash
ffmpeg -re -video_size 1920x1080 -f x11grab -draw_mouse 0 -i :0.0 -c:v libx264 \
    -pix_fmt yuv420p -preset veryfast -tune zerolatency -flags +global_header \
    -f kyber tcp://localhost:1234
```

Le module `kyber` pour FFmpeg affiche sur _stderr_ le PTS et la taille du
paquet, suivi de la représentation en hexa du header :

```
[pts=0 size=252644]	00 00 00 00 00 00 00 00 00 03 da e4
[pts=1 size=26886]	00 00 00 00 00 00 00 01 00 00 69 06
[pts=2 size=6448]	00 00 00 00 00 00 00 02 00 00 19 30
[pts=3 size=82353]	00 00 00 00 00 00 00 03 00 01 41 b1
[pts=4 size=11646]	00 00 00 00 00 00 00 04 00 00 2d 7e
[pts=5 size=21037]	00 00 00 00 00 00 00 05 00 00 52 2d
[pts=6 size=17575]	00 00 00 00 00 00 00 06 00 00 44 a7
[pts=7 size=16162]	00 00 00 00 00 00 00 07 00 00 3f 22
[pts=8 size=16434]	00 00 00 00 00 00 00 08 00 00 40 32
[pts=9 size=42124]	00 00 00 00 00 00 00 09 00 00 a4 8c
[pts=10 size=6919]	00 00 00 00 00 00 00 0a 00 00 1b 07
[pts=11 size=5661]	00 00 00 00 00 00 00 0b 00 00 16 1d
[pts=12 size=5079]	00 00 00 00 00 00 00 0c 00 00 13 d7
…
```

_Bizarrement, les PTS reçus sont incrémentés de 1 entre chaque paquet, alors que
[les paquets écrits par txproto][txproto call] ont bien des PTS liés à
l'instant de capture. Il faudrait investiguer pourquoi._

[txproto call]: https://github.com/cyanreg/txproto/blob/1e3da0b55d9694a9998eecfb3cee83faf7b10ca0/src/muxing.c#L151

Cependant, cette solution n'est pas satisfaisante : elle requiert un build
_custom_ de FFmpeg (ce qui ajoute de la complexité), alors que le code
s'occupant de la capture pourrait en théorie gérer l'envoi des paquets
lui-même.

Dit autrement, la nécessité de créer un module spécifique pour FFmpeg ne
résulte que de la contrainte de _txproto_ à utiliser FFmpeg pour le mux.


## Besoins

Dans un objectif de simplicité, ce dont nous aurions besoin pour mettre en
place l'architecture présentée au début, c'est un programme qui effectue la
capture de l'écran et/ou du son (ce que fait _txproto_) et qui expose les
paquets résultant de l'encodage de la capture et ses méta-données, pour qu'on
en fasse ce que l'on veut (en l'occurrence, les envoyer par IPC à KYMUX).

La flexibilité offerte par le scripting LUA ne me semble d'ailleurs pas
vraiment nécessaire dans notre cas (quelques paramètres en ligne de commande
pourraient être suffisants, pour paramétrer la source ou le codec utilisé, mais
bon, pourquoi pas).

Un tel programme pourrait même sortir directement les paquets sur _stdout_, que
KYMUX pourrait lire directement :

```
txcapture display --encoder=libx264 --width=1920 --height=1080 --preset=ultrafast --tune=zerolatency | hd
```

```
txcapture audio … | hd
```

_txproto_ est très souple pour la configuration des entrées/sorties et gère
le pipeline de bout en bout (en déléguant l'encodage et le mux à FFmpeg).

Mais si on décide d'effectuer la capture (par _txproto_) et le mux (par KYMUX)
dans des processus séparés, alors on ne veut pas que _txproto_ gère le pipeline
de bout en bout, et nous avons besoin d'un outil de capture qui expose les
paquets directement, sans les muxer dans un format existant qui ne serait pas
approprié à notre usage.

Comment faire ça proprement?
