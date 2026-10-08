import csv
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
rows = list(csv.reader(open(src, encoding="utf-8-sig", newline="")))
header, body = rows[0], rows[1:]

ISM = "EE_Core_Instruction_Set_Manual"
VUM = "VU_User_Manual"
ECUM = "EE_Core_User_Manual"

# AVX-512 で初めて使える命令・機能 (AVX2 に同等の単一命令がないもの)
AVX512_ONLY = re.compile(
    r"VPTERNLOG|VPERMT2|VPERMI2|VPLZCNT|VPCMPU?[BWDQ]\b|VPCMPU?D/|VPCMP[BWDQ] |VPCMP\*|VPMOVM2|VFPCLASS|"
    r"VPSRAV?Q|VPERMB|VPERMW|VPSH[LR]DV?|VRANGE|VFIXUPIMM|VGETEXP|VRCP14|VRSQRT14|"
    r"\bK(AND|OR|MOV|TEST|SHIFT|XOR)|VPROL|VPMULLQ|VPCOMPRESS|VPSCATTER|VPBLENDM|masked|"
    r"VPMOVDW|VPMOVQD|VPMOVDB|k-mask|-> k"
)

# name -> dict of overrides. Keys: cand, ext, level, exact, note, avx2, benefit, prio, fix, ref
O = {}


def ov(names, **kw):
    for n in names.split():
        O.setdefault(n, {}).update(kw)


# ---- EE MMI ----
ov("PSLLVW PSRLVW PSRAVW",
   cand="VPSLLVD/VPSRLVD/VPSRAVD + VPSHUFD(0x88) + VPMOVSXDQ",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="旧CSVの『直接候補』は誤り。対象は各64bitの下位word(word0/word2)のみで、32bit結果を64bitへ符号拡張する。可変シフト自体はAVX2で使用可能",
   ref=f"{ISM} PSLLVW/PSRLVW/PSRAVW 節")
ov("PHMSBH",
   cand="VPAND(奇数halfword mask) / VPAND(偶数halfword mask) + VPMADDWD×2 + VPSUBD",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="VPSIGNWはEVEX符号化が存在しない。偶数側を符号反転してVPMADDWDする方式は0x8000の反転が飽和せず誤る。prod=奇数積−偶数積をmask分離で計算する。HI/LOへの配置も別途",
   ref=f"{ISM} PHMSBH 節")
ov("QFSRV",
   cand="VPBROADCASTB(SA/8) + VPADDB(iota) + VPERMT2B",
   ext="AVX-512VBMI + AVX-512VL", level="短い列", avx2="AVX-512固有あり", prio="P3",
   benefit="2ソースbyte permute 1命令で256bit連結からの抽出が可能",
   fix="SAはMTSAB/MTSAHで設定されバイト/ハーフワード単位に限られるため、VBMI2のbit単位funnel shiftは不要。MTSAで任意値を入れた結果は不定なのでSA>=16は従来経路へfallback",
   ref=f"{ISM} QFSRV / MTSAB / MTSAH / MTSA 節")
ov("PCEQB PCEQH PCEQW PCGTB PCGTH PCGTW",
   cand="VPCMPEQ*/VPCMPGT* (VEX 1命令) を維持",
   ext="AVX2", level="直接候補", avx2="AVX2で同等", prio="不採用(単体)",
   fix="結果がguestのvector maskなので、VPCMP→k→VPMOVM2*は2命令になり常に悪化する。後続命令がmaskを直接使える場合だけk化を検討",
   ref=f"{ISM} PCEQ*/PCGT* 節")
ov("PCPYH",
   cand="VPSHUFLW(0x00) + VPSHUFHW(0x00)",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="旧CSVの『VPSHUFW』という命令は存在しない。各64bitの最下位halfwordを4回複写する",
   ref=f"{ISM} PCPYH 節")
ov("PABSH",
   cand="VPABSW + VPMINUW(0x7FFF)",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="0x8000→0x7FFFの飽和は、符号なしminを1命令追加するだけで実現できる。比較やmaskは不要。AVX2/AVX-512共通の改善候補",
   ref=f"{ISM} PABSH 節")
ov("PABSW",
   cand="VPABSD + VPMINUD(0x7FFFFFFF)",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="INT_MIN→INT_MAXの飽和は、符号なしminを1命令追加するだけで実現できる。AVX2/AVX-512共通の改善候補",
   ref=f"{ISM} PABSW 節")
ov("PADDUW",
   cand="VPXOR(all-ones) + VPMINUD + VPADDD  (a + min(b, ~a))",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="AVX-512のVPCMPUD+masked setと命令数は同じ。overflow時は0xFFFFFFFFへクランプ",
   ref=f"{ISM} PADDUW 節")
ov("PSUBUW",
   cand="VPMAXUD(a,b) + VPSUBD(·,b)",
   ext="AVX2", level="短い列", avx2="AVX2で同等", prio="P4",
   fix="符号なし飽和減算は2命令で実現できる。AVX-512固有命令は不要",
   ref=f"{ISM} PSUBUW 節")
ov("PLZCW",
   cand="VPSRAD(31) + VPXORD + VPLZCNTD + VPSUBD(1)",
   ext="AVX-512CD + AVX-512VL", avx2="AVX-512固有あり", prio="済(Phase1)",
   benefit="VPLZCNTD",
   fix="マニュアルの『上位に並ぶ同じ値のbit数−1』と一致。Phase 1で実装済み。high TEMP化はPhase 2の計測で不採用",
   ref=f"{ISM} PLZCW 節")
ov("PADDSW PSUBSW", avx2="AVX-512固有あり", prio="済(Phase1)", benefit="VPTERNLOGDによるoverflow選択")
ov("PPAC5 PEXT5", avx2="AVX-512固有あり", prio="P4", benefit="VPTERNLOGDによるbitfield合成")
ov("PPACB PPACH PPACW PINTH PINTEH",
   avx2="AVX-512固有あり", prio="P3",
   benefit="VPERMT2B/W/Dで2ソースpermuteを1命令化",
   fix="2ソースの固定permuteなので、VPERMT2B/W/Dで1命令(+index定数)になる。AVX2では複数のshuffle/unpack/blendが必要")
ov("PEXEH PEXCH PEXEW PEXCW PREVH PROT3W",
   avx2="AVX2で同等", prio="P4",
   fix="1ソースの固定permuteで、VPSHUFB/VPSHUFD/VPSHUFLW/HWで足りる。VPERMWの利点はない")
ov("DIV1 DIVU1 PDIVW PDIVUW PDIVBW", avx2="scalar推奨", prio="不採用")

# ---- VU float ----
for p in ("", "V"):
    for n in ("0", "4", "12", "15"):
        ov(f"{p}ITOF{n}",
           cand="VCVTDQ2PS (MXCSR=RZ) + VMULPS(2^-n)",
           ext="AVX (VEX)", level="短い列", exact="exact可（VU丸め設定=Chop時）",
           avx2="AVX2で同等", prio="P4",
           fix="{rz-sae}はpacked 128bitでは符号化できない(scalar/ZMM reg-regのみ)。マニュアル上、ITOFは0方向切捨てで|x|の上位24bitを仮数にするので、RZのCVTDQ2PSと一致する。2^-n倍も誤差なし。旧CSVの『要補正』は過剰",
           ref=f"{VUM} 2.2 丸め処理")
        ov(f"{p}FTOI{n}",
           cand="VMULPS(2^n) + VCVTTPS2DQ + 正方向overflow fixup(元入力の符号で0x7FFFFFFF/0x80000000選択)",
           level="短い列", exact="exact可（fixup込み）", avx2="AVX2で同等", prio="P4",
           fix="変換overflowは±MAXへ飽和し、フラグは不変。CVTTPS2DQは正方向overflowでも0x80000000を返すので補正が必要。E=255入力(IEEEではNaN/Inf)も大きな値として飽和する。fixupは中間結果ではなく元入力の符号で判定すること",
           ref=f"{VUM} 2.1.1 / 2.3 例外処理")
for p in ("", "V"):
    for base in ("MAX", "MINI"):
        for s in ("", "i", "bc"):
            ov(f"{p}{base}{s}",
               cand="符号-絶対値の整数比較 (VPMAXSD/VPMINSD + 負数補正)。VMAXPS/VMINPSは不可",
               exact="exact可（±0の扱いは実機確認要）", avx2="AVX2で同等", prio="P4",
               fix="VUにNaN/Infはなく、E=255は通常の大きな値。VMAXPS/VMINPSはE=255のbitパターンをNaN扱いするため誤る。マニュアルは単純な『>』比較で、等しい場合はftを選ぶ",
               ref=f"{VUM} 2.1.1 / MAX・MINI 節")
for p in ("", "V"):
    ov(f"{p}CLIP",
       cand="VPANDD(abs) + VPERMD/VPSHUFD(w broadcast) + VPCMPD(整数GT) → k + KMOVW/KSHIFT → CF更新",
       ext="AVX-512F + AVX-512VL", level="複数命令", exact="exact可（整数比較）",
       avx2="AVX-512固有あり", prio="P2",
       benefit="比較結果をk-maskから直接flag bit列へ変換できる",
       fix="VCMPPSはE=255をNaN扱いするので不可。絶対値どうしの整数比較で正しく順序付けできる。結果がCFのbit列なのでk-mask化の効果が出やすい",
       ref=f"{VUM} 2.1.1 / CLIP 節 / 3.3.3")
FMA_FIX = ("FMA(1回丸め)は不可。マニュアルでは乗算段と加算段がそれぞれ例外・クランプ・フラグを持つ。"
           "VMULPS→乗算段補正→VADDPS/VSUBPS→加算段補正を維持する")
for p in ("", "V"):
    for base in ("MADD", "MSUB", "MADDA", "MSUBA"):
        for s in ("", "i", "q", "bc"):
            ov(f"{p}{base}{s}",
               cand=("VMULPS + VADDPS (段ごとに補正)" if "ADD" in base else "VMULPS + VSUBPS (段ごとに補正)")
               if not s else
               (("VBROADCASTSS(I) + " if s == "i" else "VBROADCASTSS(Q) + " if s == "q" else "VPSHUFD(component broadcast) + ")
                + ("VMULPS + VADDPS (段ごとに補正)" if "ADD" in base else "VMULPS + VSUBPS (段ごとに補正)")),
               avx2="AVX2で同等", prio=("P3" if p else "P1(VU1 micro residency経由)"),
               fix=FMA_FIX, ref=f"{VUM} 2.2 / 3.3.6 MADD・MSUB命令でのフラグ変化", vu_only=True)
    ov(f"{p}OPMSUB", cand="VPSHUFD + VMULPS + VSUBPS (段ごとに補正)", avx2="AVX2で同等",
       prio=("P3" if p else "P1(VU1 micro residency経由)"), fix=FMA_FIX, ref=f"{VUM} 3.3.6")
ov("MADD.S MSUB.S MADDA.S MSUBA.S",
   cand="VMULSS + VADDSS/VSUBSS (段ごとに補正)", avx2="AVX2で同等", prio="P4",
   fix=FMA_FIX + "。EE FPUも同様", ref=f"{ECUM} 8.7 丸め処理")
for n in ("ADD.S", "SUB.S", "MUL.S", "DIV.S", "SQRT.S", "ADDA.S", "SUBA.S", "MULA.S"):
    ov(n, avx2="AVX2で同等", prio="P4",
       fix="{rz-sae}は不要。scalarなら符号化できるが、MXCSRは既にRZで、guard bitなしのPS2切捨てとはIEEE RZでも最下位bitが一致しない。exactはSoftFloat経路で扱う",
       ref=f"{ECUM} 8.7 丸め処理")
ov("CVT.S.W", cand="VCVTSI2SS/VCVTDQ2PS (MXCSR=RZ)", exact="exact可（RZ時）", avx2="AVX2で同等", prio="P4",
   fix="{rz-sae}は不要。0方向切捨てはRZ変換と一致する(要テスト)", ref=f"{ECUM} 8.7")
ov("CVT.W.S", avx2="AVX2で同等", prio="P4",
   fix="正方向overflowの0x7FFFFFFF補正が必要。元入力の符号で判定する")
ov("MAX.S MIN.S", cand="符号-絶対値の整数比較", avx2="AVX2で同等", prio="P4",
   fix="E=255はNaNではないので、VMAXSS/VMINSSは不可")
ov("C.EQ.S C.LT.S C.LE.S", cand="整数比較(符号-絶対値変換) + scalar flag", avx2="AVX2で同等", prio="P4",
   fix="VCMPSSはE=255をNaN扱いするので不可。単一の比較結果なのでk-maskの利点はない")
for n in ("ERCPR", "ERSQRT", "ERLENG", "ERSADD"):
    ov(n, avx2="AVX2で同等", prio="P4",
       fix="VRCP14/VRSQRT14は2^-14の近似でbit-exactにならない。使うならNewton初期値に限り、結果はPS2 EFUの再現と照合する")
ov("FCAND FMAND FSAND FCOR FCEQ FCSET FCGET FMEQ FMOR FSEQ FSOR FSSET",
   avx2="scalar推奨", prio="不採用", fix="flag registerはscalarで十分。k-maskは結果をvectorで生成するときだけ意味がある")
ov("ILW ILWR ISW ISWR VILWR VISWR",
   cand="scalar MOV", avx2="scalar推奨", prio="不採用",
   fix="1要素のアクセスなので、gather/scatterは遅いだけ")

ov("B BAL IBEQ IBGEZ IBGTZ IBLEZ IBLTZ IBNE JALR JR", cand="scalar CMP + Jcc", avx2="scalar推奨", prio="不採用",
   fix="VIは16bit整数でx86 GPRに置かれる。分岐はscalarで処理する", vu_only=True)
ov("MFIR MTIR VMFIR VMTIR", avx2="AVX2で同等", prio="P4",
   fix="VIはx86 GPRにあるので、MOVD/PINSRDとVPMOVSXWDで足りる。VPMOVDWの利点はない")
ov("RINIT RGET RNEXT RXOR VRINIT VRGET VRNEXT VRXOR", cand="scalar整数演算", avx2="scalar推奨", prio="不採用",
   fix="Rレジスタは1要素なので、vector化の利点はない")
ov("RSQRT VRSQRT RSQRT.S", avx2="AVX2で同等",
   fix="VRSQRT14は近似なのでbit-exactにならない。VSQRTSS+VDIVSSに、PS2の補正を加える")
ov("LWC1 SWC1", cand="VMOVD / MOVSS", avx2="AVX2で同等", prio="P4", fix="1要素のload/storeなのでmaskの利点はない")
ov("LQ LQD LQI VLQD VLQI MOVE MFP VMOVE", avx2="AVX2で同等", prio="P4",
   fix="dest field maskはVBLENDPS(imm) 1命令で表現できる。EVEX masked moveでも命令数は減らない")
ov("SQ SQD SQI VSQD VSQI", avx2="AVX-512固有あり", benefit="部分dest maskのstoreをmasked VMOVDQU32 1命令にできる",
   fix="dest maskが部分的な場合に限り、AVX2のload+blend+storeをmasked storeで置換できる。全成分storeはVEXのまま")
ov("PMFHI PMFLO PMTHI PMTLO", cand="VMOVDQA (レジスタ配置次第)", avx2="AVX2で同等", prio="P4", fix="単純な128bit転送")
ov("PMTHL.LW", cand="VPSHUFD + VPBLENDD", avx2="AVX2で同等", prio="P4", fix="word単位のblendはAVX2のVPBLENDDで足りる")
ov("PADSBH", cand="VPADDW + VPSUBW + VPBLENDD(imm)", avx2="AVX2で同等", prio="P4",
   fix="下位4halfwordがsub、上位4halfwordがadd。固定位置なのでimm blendで足りる")
ov("PNOR", avx2="AVX-512固有あり", prio="P4", benefit="VPTERNLOGで1命令化(AVX2はPOR+PXOR)")

# ---- category defaults ----
CAT_PRIO = {
    "EE CPU": ("scalar推奨", "不採用", "EE GPRは主にx86 GPRに常駐するため、vector化の利点はない"),
    "EE COP0": ("対象外", "不採用", ""),
}

new_header = header[:]
new_header[3] = "推奨候補(v2)"
new_header[4] = "必要拡張(v2)"
new_header += ["AVX2可否", "AVX-512固有の利点", "静的優先度(目安)", "v2修正内容", "根拠(公式マニュアル)"]

out = []
changed = 0
for r in body:
    r = r + [""] * (len(header) - len(r))
    cat, name = r[0], r[1]
    o = O.get(name, {})
    if cat.startswith("EE") and o.get("vu_only"):
        o = {}
    if cat.startswith("EE") and name in ("LQ", "SQ"):
        o = {"cand": "VMOVDQA/VMOVDQU (128bit)", "avx2": "AVX2で同等", "prio": "P4",
             "fix": "EEの128bit load/storeにはdest maskがない。maskの利点はない"}
    row = r[:]
    if "cand" in o:
        row[3] = o["cand"]
    if "ext" in o:
        row[4] = o["ext"]
    if "level" in o:
        row[5] = o["level"]
    if "exact" in o:
        row[6] = o["exact"]
    avx2 = o.get("avx2")
    prio = o.get("prio")
    fix = o.get("fix", "")
    benefit = o.get("benefit", "")
    if cat in CAT_PRIO:
        a, p, f = CAT_PRIO[cat]
        avx2 = avx2 or a
        prio = prio or p
        fix = fix or f
    if not avx2:
        if row[5].startswith("なし") or row[3].startswith("なし"):
            avx2 = "対象外"
        elif AVX512_ONLY.search(row[3]):
            avx2 = "AVX-512固有あり"
        else:
            avx2 = "AVX2で同等"
    if "ext" not in o:
        orig = row[4]
        if avx2 == "AVX2で同等":
            row[4] = "AVX2" + (f"（EVEX版: {orig}）" if orig and orig != "なし" and not orig.startswith("AVX2") else "")
        elif avx2 == "scalar推奨":
            row[4] = "x86-64 scalar"
    if avx2 == "AVX-512固有あり" and not benefit:
        m = AVX512_ONLY.search(row[3])
        benefit = m.group(0).strip() if m else ""
    if not prio:
        if avx2 in ("対象外", "scalar推奨"):
            prio = "不採用"
        elif cat == "VU micro Upper":
            prio = "P1(VU1 micro residency経由)"
        elif cat == "VU micro Lower":
            prio = "P2"
        elif cat == "VU macro":
            prio = "P3"
        else:
            prio = "P4" if avx2 == "AVX2で同等" else "P3"
    if o:
        changed += 1
    out.append(row + [avx2, benefit, prio, fix, o.get("ref", "")])

with open(dst, "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f, lineterminator="\n")
    w.writerow(new_header)
    w.writerows(out)

from collections import Counter
print("rows", len(out), "overridden", changed)
print(Counter(r[-5] for r in out))
print(Counter(r[-3] for r in out))
missing = [n for n in O if n not in {r[1] for r in body}]
print("override names not in CSV:", missing)
