param(
    [Parameter(Mandatory = $true)][string]$Mapping,
    [string]$Output = "$PSScriptRoot/instruction-audit.csv"
)
$ErrorActionPreference = 'Stop'
$rows = Import-Csv -LiteralPath $Mapping -Encoding utf8
$result = foreach ($row in $rows) {
    $category = $row.'区分'
    $instruction = $row.'PS2命令'
    $level = $row.'対応レベル'
    $decision = '既存経路維持'
    $reason = ''
    $source = ''
    if ($category -eq 'EE CPU') {
        $source = 'pcsx2/x86/ix86-32/'
        $reason = '単一GPRの演算・分岐・例外・メモリアクセスは既存スカラーJITを維持。ベクトル化する独立した複数命令はない。'
    } elseif ($category -eq 'EE COP0') {
        $source = 'pcsx2/x86/iCOP0.cpp'
        $reason = 'COP0の状態更新、TLB、例外、同期処理を維持。SIMD置換は主処理を表現しない。'
    } elseif ($category -like 'EE MMI*') {
        $source = 'pcsx2/x86/iMMI.cpp;pcsx2/MMI.cpp'
        $reason = '128bitの既存SSE/VEX演算を維持。同じ演算のEVEX化だけではレーン数・命令数は減らず、HI/LO配置と飽和補正は必要。'
        if ($instruction -in @('PLZCW', 'PNOR', 'PEXT5', 'PPAC5', 'QFSRV')) {
            $decision = 'AVX512経路実装'
            $source += ';pcsx2/x86/InterpreterAVX512.cpp;pcsx2/x86/MMIAVX512.inl'
            $reason = switch ($instruction) {
                'PLZCW' { 'VPLZCNTDで符号ビット数を計算。JITとInterpreter、Rd上位64bit保持。' }
                'PNOR' { 'VPTERNLOGDでNORを合成。JITとInterpreter、全レジスタaliasを検証。' }
                'PEXT5' { 'Interpreterのビット組合せをVPTERNLOGDへ。JITの既存128bitシフト列を維持。' }
                'PPAC5' { 'Interpreterのビット組合せとJITの最終ビット選択をVPTERNLOGDへ。' }
                'QFSRV' { 'Interpreterの有効shiftはVBMI二入力byte permute。無効shiftは既存処理。JITの連結メモリloadを維持。' }
            }
        } elseif ($instruction -in @('PADDSW', 'PSUBSW')) {
            $decision = 'AVX512経路実装'
            $reason = 'JITの既存overflow判定と飽和值を維持し、最終合成をVPTERNLOGDへ。'
        }
    } elseif ($category -eq 'EE COP1 / FPU') {
        $source = 'pcsx2/x86/iFPU.cpp;pcsx2/x86/microVU_Lower.inl'
        $reason = '既存命令・補正を維持。IEEEのNaN/Inf/denormal・丸め・FMAをPS2演算へ無条件に適用しない。'
        if ($instruction -match '^(ADD|SUB|MUL|MADD|MSUB|DIV|SQRT|RSQRT)') {
            $decision = '共通AVX512厳密kernel経路'
            $reason = '厳密演算モードの共通生成kernelで拡張scratch/maskを利用。ACCとflagsの既存意味論を維持。native演算モードは既存経路。'
        }
    } elseif ($category -like 'VU*') {
        $source = 'pcsx2/x86/microVU_Upper.inl;pcsx2/x86/microVU_UpperSoft.inl;pcsx2/x86/microVU_Lower.inl'
        $operation = $instruction
        if ($category -eq 'VU macro') { $operation = $operation -replace '^V', '' }
        $reason = '既存のレーンマスク、VF/VI、flags、Q/P、同期と状態更新を維持。128bit演算を広幅化する独立レーンはない。'
        if ($operation -match '^(ADD|SUB|MUL|MADD|MSUB|DIV|SQRT|RSQRT|OPMULA|OPMSUB)') {
            $decision = '共通AVX512厳密kernel経路'
            $reason = '厳密演算モードの共通FMAC/下位Q生成kernelにAVX512経路。broadcast/ACC/各maskとmacro wrapperは既存処理を共有。'
        } elseif ($operation -match '^E') {
            $reason = 'EFUの演算列・丸め順・P更新を維持。近似命令やFMAへの置換の一致性は確立していないため採用しない。'
        } elseif ($operation -match '^(MIN|MAX|CLIP)') {
            $reason = '既存比較・clamp処理を維持。VRANGEPS保留。PS2の拡張有限指数をIEEE NaNとして処理しない。'
        }
    } else { throw "Unknown mapping category: $category" }
    if ($level -eq 'なし') {
        $decision = '既存制御処理維持'
        $reason = '命令表にSIMD主演算候補なし。既存の状態・例外・同期処理を維持。'
    }
    [pscustomobject]@{
        '区分' = $category
        'PS2命令' = $instruction
        'AVX512候補' = $row.'AVX-512候補'
        '判断' = $decision
        '理由' = $reason
        '実装箇所' = $source
        'マニュアル' = $row.'マニュアル'
        '行' = $row.'行'
    }
}
$result | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding utf8
$result | Group-Object '判断' | Select-Object Count, Name
