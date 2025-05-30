#include "mdp.hpp"
#include "log.hpp"

#define _USE_MATH_DEFINES
#include <cmath>
#include <boost/math/special_functions/bessel.hpp>
#include <numeric>

namespace OScofo {

/*
    // ──────────────────────────────── REFERENCES ───────────────────────────────────────

    * GONG, R.; CUVILLIER, P.; OBIN, N.; CONT, A. Real-Time Audio-to-Score Alignment of Sin-
        ging Voice Based on Melody and Lyric Information. In: Interspeech, 2015, Dresde, Germany.
        Anais… [S.l.: s.n.], 2015.

    * CONT, A. A Coupled Duration-Focused Architecture for Real-Time Music-to-Score Align-
        ment. IEEE Transactions on Pattern Analysis and Machine Intelligence, [S.l.], v.32, n.6,
        p.974–987, 2010.

    * CONT, A. Improvement of Observation Modeling for Score Following. 2004.

    * GUÉDON, Y. Hidden Hybrid Markov/Semi-Markov Chains. Computational Statistics & Data
        Analysis, [S.l.], v.49, n.3, p.663–688, 2005.

    * LARGE, E. W.; JONES, M. R. The Dynamics of Attending: How People Track Time-Varying
        Events. Psychological Review, [S.l.], v.106, n.1, p.119–159, 1999.

    * LARGE, E. W.; PALMER, C. Perceiving Temporal Regularity in Music. Cognitive Science,
        [S.l.], v.26, n.1, p.1–37, 2002.
*/

// ╭─────────────────────────────────────╮
// │Constructor and Destructor Functions │
// ╰─────────────────────────────────────╯
MDP::MDP(double Sr, double FFTSize, double HopSize) {
    m_HopSize = HopSize;
    m_FFTSize = FFTSize;
    m_Sr = Sr;

    m_SyncStrength = 0.5;
    m_PhaseCoupling = 0.5;
    m_BlockDur = (1 / m_Sr) * HopSize;
    m_TimeInPrevEvent = 0;

    SetTunning(440);
}

// ╭─────────────────────────────────────╮
// │               Errors                │
// ╰─────────────────────────────────────╯
bool MDP::HasErrors() {
    return m_HasErrors;
}

// ─────────────────────────────────────
std::vector<std::string> MDP::GetErrorMessage() {
    return m_Errors;
}

// ─────────────────────────────────────
void MDP::SetError(const std::string &message) {
    printf("Error: %s.\n", message.c_str());
    m_HasErrors = true;
    m_Errors.push_back(message);
}

// ─────────────────────────────────────
void MDP::ClearError() {
    m_HasErrors = false;
    m_Errors.clear();
}

// ─────────────────────────────────────
ActionVec MDP::GetEventActions(int Index) {
    if (Index < 0 || Index >= (int)m_States.size()) {
        return ActionVec();
    }

    MacroState State = m_States[(size_t)Index];
    return State.Actions;
}

// ─────────────────────────────────────
void MDP::SetScoreStates(States ScoreStates) {
    if (ScoreStates.size() == 0) {
        SetError("ScoreStates is empty, add some events to the score");
        return;
    }

    m_States.clear();
    m_States = ScoreStates;

    for (MacroState &State : m_States) {
        State.Obs.resize(m_BufferSize + 1, 0);
        State.Forward.resize(m_BufferSize + 1, 0);
        for (AudioState &MicroState : State.SubStates) {
            MicroState.Obs.resize(m_BufferSize + 1, 0);
            MicroState.Forward.resize(m_BufferSize + 1, 0);
        }
    }

    m_CurrentStateIndex = -1;
    m_Kappa = 1;
    m_BPM = m_States[0].BPMExpected;
    m_PsiN = (double)60.0f / m_States[0].BPMExpected;
    m_PsiN1 = (double)60.0f / m_States[0].BPMExpected;
    m_LastPsiN = (double)60.0f / m_States[0].BPMExpected;
    m_BeatsAhead = m_States[0].BPMExpected / 60 * m_SecondsAhead;
    m_CurrentStateIndex = -1;
    m_SyncStr = 0;

    UpdateAudioTemplate();
    UpdatePhaseValues();
}

// ─────────────────────────────────────
void MDP::BuildPitchTemplate(double Freq) {
    // Following Gong (2015), eq 5 and 6
    const double sigmaSemitons = m_PitchTemplateSigma;
    const double sigmaLog = sigmaSemitons / 12.0;
    const double beta = 0.5;

    double rootBinFreq = round(Freq / (m_Sr / m_FFTSize));
    if (m_PitchTemplates.find(rootBinFreq) != m_PitchTemplates.end()) {
        return;
    }

    m_PitchTemplates[rootBinFreq].resize(m_FFTSize / 2, 0.0);
    for (int k = 1; k <= m_Harmonics; ++k) {
        double harmonicFreqHz = Freq * k;
        double sigmaHz = harmonicFreqHz * (std::pow(2.0, sigmaLog) - 1.0);
        double envelope = std::exp(-beta * (k - 1));
        for (size_t i = 0; i < m_FFTSize / 2; ++i) {
            double binFreq = i * (m_Sr / static_cast<double>(m_FFTSize));
            double exponent = -0.5 * std::pow((binFreq - harmonicFreqHz) / sigmaHz, 2);
            double gaussian = (1.0 / (sigmaHz * std::sqrt(2 * M_PI))) * std::exp(exponent);
            m_PitchTemplates[rootBinFreq][i] += envelope * gaussian;
        }
    }

    // Normalize template to sum to 1 (probability distribution)
    double sum = std::accumulate(m_PitchTemplates[rootBinFreq].begin(), m_PitchTemplates[rootBinFreq].end(), 0.0);
    if (sum > 0) {
        for (auto &val : m_PitchTemplates[rootBinFreq]) {
            val = (val + 1e-12) / (sum + 1e-12); // Avoid zero probabilities
        }
    }
}

// ─────────────────────────────────────
void MDP::UpdateAudioTemplate() {
    int StateSize = (int)m_States.size();
    m_PitchTemplates.clear();

    for (int h = 0; h < StateSize; h++) {
        if (m_States[h].Type == NOTE || m_States[h].Type == TRILL) {
            for (AudioState &SubState : m_States[h].SubStates) {
                if (SubState.Type == NOTE) {
                    BuildPitchTemplate(SubState.Freq);
                }
            }
        }
    }
}

// ─────────────────────────────────────
std::unordered_map<double, PitchTemplateArray> MDP::GetPitchTemplate() {
    if (m_PitchTemplates.size() == 0) {
        SetError("PitchTemplates is empty, please report this issue");
        return m_PitchTemplates;
    }
    return m_PitchTemplates;
}

// ─────────────────────────────────────
void MDP::UpdatePhaseValues() {
}

// ╭─────────────────────────────────────╮
// │          Set|Get Functions          │
// ╰─────────────────────────────────────╯
void MDP::ClearStates() {
    m_States.clear();
}
// ─────────────────────────────────────
double MDP::GetLiveBPM() {
    return m_BPM;
}

// ─────────────────────────────────────
double MDP::GetKappa() {
    return m_Kappa;
}

// ─────────────────────────────────────
void MDP::SetBPM(double BPM) {
    m_BPM = BPM;
}

// ─────────────────────────────────────
void MDP::SetdBTreshold(double dB) {
    m_dBTreshold = dB;
}

// ─────────────────────────────────────
void MDP::SetTunning(double Tunning) {
    m_Tunning = Tunning;
}

// ─────────────────────────────────────
void MDP::SetHarmonics(int Harmonics) {
    m_Harmonics = Harmonics;
}

// ─────────────────────────────────────
void MDP::SetMinEntropy(double EntropyValue) {
    m_MinEntropy = EntropyValue;
}

// ─────────────────────────────────────
int MDP::GetTunning() {
    return m_Tunning;
}

// ─────────────────────────────────────
void MDP::SetCurrentEvent(int Event) {
    m_CurrentStateIndex = Event;
    m_Tau = 0;
}

// ─────────────────────────────────────
int MDP::GetStatesSize() {
    return m_States.size();
}
// ─────────────────────────────────────
void MDP::AddState(MacroState State) {
    m_States.push_back(State);
}
// ─────────────────────────────────────
MacroState MDP::GetState(int Index) {
    return m_States[Index];
}

// ─────────────────────────────────────
void MDP::SetPitchTemplateSigma(double f) {
    m_PitchTemplateSigma = f;
}

// ╭─────────────────────────────────────╮
// │            Time Decoding            │
// ╰─────────────────────────────────────╯
double MDP::InverseA2(double SyncStrength) {
    // SyncStrength must be between 0 and 1
    if (SyncStrength < 0) {
        return 0;
    }

    // Following Large and Jones (1999, p. 157).
    if (SyncStrength > 0.95) {
        return 10.0f;
    }

    double Low = 0.0;
    double Tol = 1e-16;
    double High = std::max(SyncStrength, 10.0);
    double Mid;

    // In my tests I never reached more than 100 iterations.
    int i;
    for (i = 0; i < 1000; ++i) {
        Mid = (Low + High) / 2.0;
        double I1 = boost::math::cyl_bessel_i(1, Mid);
        double I0 = boost::math::cyl_bessel_i(0, Mid);
        double A2Mid = I1 / I0;
        if (std::fabs(A2Mid - SyncStrength) < Tol) {
            return Mid;
        } else if (A2Mid < SyncStrength) {
            Low = Mid;
        } else {
            High = Mid;
        }
    }
    LOGE() << "InverseA2 not converged after " << i << " iterations.";
    return Mid;
}

// ─────────────────────────────────────
double MDP::CouplingFunction(double Phi, double PhiMu, double Kappa) {
    // Equation 2b from Large and Palmer (2002)
    double ExpKappa = exp(Kappa);
    double PhiNDiff = Phi - PhiMu;
    double CosTerm = cos(TWO_PI * PhiNDiff);
    double SinTerm = sin(TWO_PI * PhiNDiff);
    double PhiN = (1 / (TWO_PI * ExpKappa)) * exp(Kappa * CosTerm) * SinTerm;
    return PhiN;
}

// ─────────────────────────────────────
double MDP::ModPhases(double Phase) {
    // Following Cont (2010) conventions
    Phase = std::fmod(Phase + M_PI, TWO_PI);
    if (Phase < 0) {
        Phase += TWO_PI;
    }
    return Phase - M_PI;
}

// ─────────────────────────────────────
int MDP::GetMaxJIndex(int StateIndex) {
    if (StateIndex == -1) {
        return 1;
    }

    double TimeInCurrEvt = m_States[StateIndex].Duration - (m_TimeInPrevEvent + m_BlockDur);
    double EventOnset = TimeInCurrEvt;
    int MaxJ = StateIndex + 1;
    for (size_t i = StateIndex + 1; i < m_States.size(); i++) {
        if (EventOnset > m_SecondsAhead) {
            MaxJ = i;
            break;
        } else {
            EventOnset += m_States[i].Duration;
        }
    }

    return MaxJ;
}

// ─────────────────────────────────────
double MDP::UpdatePsiN(int StateIndex) {
    if (StateIndex == m_CurrentStateIndex) {
        m_TimeInPrevEvent += m_BlockDur;
        m_Tau += 1;
        return m_PsiN;
    } else {
        if (StateIndex == 0) {
            double PsiK = 60 / m_States[0].BPMExpected;
            m_LastPsiN = PsiK;
            m_PsiN = PsiK;
            m_PsiN1 = PsiK;
            m_States[0].OnsetObserved = 0;
            m_BPM = m_States[0].BPMExpected;
            m_CurrentStateOnset = 0;
            m_LastTn = 0;
            m_TimeInPrevEvent = 0;
            m_Tau = 0;
            return m_PsiN;
        } else {
            m_TimeInPrevEvent += m_BlockDur;
            m_LastTn = m_CurrentStateOnset;
            m_CurrentStateOnset += m_TimeInPrevEvent;
        }
    }

    // Cont (2010), Large and Palmer (1999) and Large and Jones (2002)
    MacroState &LastState = m_States[StateIndex - 1];
    MacroState &CurrentState = m_States[StateIndex];
    MacroState &NextState = m_States[StateIndex + 1];

    double IOISeconds = m_CurrentStateOnset - m_LastTn;
    double LastPhiN = LastState.IOIPhiN;
    double LastHatPhiN = LastState.IOIHatPhiN;
    double HatPhiN = CurrentState.IOIHatPhiN;
    double PhiNExpected = LastPhiN + ((m_CurrentStateOnset - m_LastTn) / m_PsiN);
    CurrentState.IOIHatPhiN = PhiNExpected;
    CurrentState.OnsetObserved = m_CurrentStateOnset;

    // Update Variance (Cont, 2010) - Coupling Strength (Large 1999)
    double PhaseDiff = (IOISeconds / m_PsiN) - HatPhiN;
    double SyncStrength = m_SyncStr - m_SyncStrength * (m_SyncStr - cos(TWO_PI * PhaseDiff));
    double Kappa = InverseA2(SyncStrength);
    m_SyncStr = SyncStrength;
    m_Kappa = Kappa;

    // Update and Correct PhiN
    double FValueUpdate = CouplingFunction(LastPhiN, LastHatPhiN, Kappa);
    double PhiN = LastPhiN + (IOISeconds / m_LastPsiN) + (m_PhaseCoupling * FValueUpdate);
    PhiN = ModPhases(PhiN);
    CurrentState.PhaseObserved = PhiN;

    // Prediction for next PsiN+1
    double FValuePrediction = CouplingFunction(PhiN, HatPhiN, Kappa);
    double PsiN1 = m_PsiN * (1 + m_SyncStrength * FValuePrediction);

    // Prediction for Next HatPhiN
    double Tn1 = m_CurrentStateOnset + CurrentState.Duration * PsiN1;
    double PhiN1 = ModPhases((Tn1 - m_CurrentStateOnset) / PsiN1);
    NextState.IOIHatPhiN = PhiN1;

    // Update all next expected onsets
    NextState.OnsetExpected = Tn1;
    double LastOnsetExpected = Tn1;

    // the m_CurrentEvent + 1 already updated, now
    // we update the future events to get the Sojourn Time
    for (int i = m_CurrentStateIndex + 2; i < m_CurrentStateIndex + 20; i++) {
        if ((size_t)i >= m_States.size()) {
            break;
        }
        MacroState &FutureState = m_States[i];
        MacroState &PreviousFutureState = m_States[(i - 1)];
        double Duration = PreviousFutureState.Duration;
        double FutureOnset = LastOnsetExpected + Duration * PsiN1;

        FutureState.OnsetExpected = FutureOnset;
        LastOnsetExpected = FutureOnset;
    }

    // Update Values for next calls
    m_BPM = 60.0f / m_PsiN;
    m_LastPsiN = m_PsiN;

    if (StateIndex != m_CurrentStateIndex) {
        m_TimeInPrevEvent = 0;
        m_Tau = 0;
    }
    return PsiN1;
}

// ╭─────────────────────────────────────╮
// │     Markov Description Process      │
// ╰─────────────────────────────────────╯
void MDP::GetAudioObservations(int FirstStateIndex, int LastStateIndex, int T) {
    std::unordered_map<double, double> PitchObs;

    for (int j = FirstStateIndex; j <= LastStateIndex; j++) {
        if (j < 0) {
            continue;
        }

        MacroState &StateJ = m_States[j];
        int BufferIndex = (T % m_BufferSize);
        if (StateJ.Type == NOTE) {
            // TODO: Need to rethink this
            double KL = 0;
            for (AudioState AudioState : StateJ.SubStates) {
                if (PitchObs.find(AudioState.Freq) != PitchObs.end()) {
                    AudioState.Obs[BufferIndex] = PitchObs[AudioState.Freq];
                    KL = PitchObs[AudioState.Freq];
                    continue;
                }
                if (AudioState.Type == NOTE) {
                    KL = GetPitchSimilarity(AudioState.Freq);
                    PitchObs[AudioState.Freq] = KL;
                    AudioState.Obs[BufferIndex] = KL;
                }
            }
            StateJ.Obs[BufferIndex] = KL;

        } else if (StateJ.Type == REST) {
            // StateJ.Obs[BufferIndex] = Desc.Amp;

        } else if (StateJ.Type == TRILL) {
            double bestProb = 0;
            for (AudioState AudioState : StateJ.SubStates) {
                if (PitchObs.find(AudioState.Freq) != PitchObs.end()) {
                    AudioState.Obs[BufferIndex] = PitchObs[AudioState.Freq];
                    continue;
                }
                double KL = GetPitchSimilarity(AudioState.Freq);
                if (KL > bestProb) {
                    bestProb = KL;
                }
                AudioState.Obs[BufferIndex] = KL;
            }
            StateJ.Obs[BufferIndex] = bestProb;
        }
    }
}

// ─────────────────────────────────────
double MDP::GetPitchSimilarity(double Freq) {
    double KLDiv = 0.0;
    double RootBinFreq = round(Freq / (m_Sr / m_FFTSize));
    PitchTemplateArray PitchTemplate;

    if (m_PitchTemplates.find(RootBinFreq) != m_PitchTemplates.end()) {
        PitchTemplate = m_PitchTemplates[RootBinFreq];
    } else {
        BuildPitchTemplate(Freq);
        PitchTemplate = m_PitchTemplates[RootBinFreq];
    }

    for (size_t i = 0; i < m_FFTSize / 2; i++) {
        double P = PitchTemplate[i];
        double Q = m_Desc.NormSpectralPower[i];
        if (P > 0 && Q > 0) {
            KLDiv += P * log(P / Q);
        } else if (P == 0 && Q >= 0) {
            KLDiv += Q;
        }
    }

    // KLDiv /= (m_Desc.StdDev + 1e-9);
    double noise_robustness = 1.0 / (1.0 + m_Desc.StdDev);
    KLDiv *= noise_robustness;

    KLDiv = exp(-m_PitchScalingFactor * KLDiv);
    return KLDiv;
}

// ─────────────────────────────────────
std::vector<double> MDP::GetInitialDistribution() {
    int Size = m_MaxScoreState - m_CurrentStateIndex;
    std::vector<double> InitialProb(Size);

    double Dur = 0;
    double Sum = 0;

    for (int i = 0; i < Size; i++) {
        double DurProb = exp(-1 * (Dur / m_BeatsAhead));
        InitialProb[i] = DurProb;
        Dur += m_States[m_CurrentStateIndex + i].Duration; // Accumulate duration
        Sum += DurProb;
    }

    // Normalize
    for (int i = 0; i < Size; i++) {
        InitialProb[i] /= Sum;
    }

    return InitialProb;
}

// ─────────────────────────────────────
double MDP::GetTransProbability(int i, int j) {
    // simplest markov
    if (i + 1 == j) {
        return 1;
    } else {
        return 0;
    }
}

// ─────────────────────────────────────
double MDP::GetSojournTime(MacroState &State, int u) {
    double T = m_LastTn + (m_BlockDur * u);
    double Duration = State.Duration;
    double Sojourn = std::exp(-(T - m_LastTn) / (m_PsiN1 * Duration));
    return Sojourn;
}

// ─────────────────────────────────────
int MDP::GetMaxUForJ(MacroState &StateJ) {
    double MaxU = StateJ.Duration / m_BlockDur;
    int MaxUInt = round(MaxU);
    return MaxUInt;
}

// ─────────────────────────────────────
double MDP::SemiMarkov(MacroState &StateJ, int CurrentState, int j, int T, int bufferIndex) {
    if (T == 0) {
        return StateJ.Obs[bufferIndex] * GetSojournTime(StateJ, T + 1) * StateJ.InitProb;
    } else {
        double Obs = StateJ.Obs[bufferIndex];
        double MaxAlpha = -std::numeric_limits<double>::infinity();
        for (int u = 1; u <= std::min(T, GetMaxUForJ(StateJ)); u++) {
            double ProbPrevObs = 1.0;
            for (int v = 1; v < u; v++) {
                int PrevIndex = (bufferIndex - v + m_BufferSize) % m_BufferSize;
                ProbPrevObs *= StateJ.Obs[PrevIndex];
            }
            double Sur = GetSojournTime(StateJ, u);
            double MaxTrans = -std::numeric_limits<double>::infinity();
            for (int i = CurrentState; i <= j; i++) {
                if (i < 0) {
                    continue;
                }
                MacroState &StateI = m_States[i];
                int PrevIndex = (T - u) % m_BufferSize;
                if (i != j) {
                    MaxTrans = std::max(MaxTrans, GetTransProbability(i, j) * StateI.Forward[PrevIndex]);
                } else {
                    // for (AudioState &SubState : StateJ.SubStates) {

                    // if (SubState.Markov == MARKOV) {
                    //     int StateSize = StateJ.SubStates.size();
                    //     // SubState.Forward[bufferIndex] = Markov(SubState, CurrentState, j, T, bufferIndex);
                    // }
                    // }

                    MaxTrans = std::max(MaxTrans, StateJ.Forward[PrevIndex]);
                }
            }

            double MaxResult = ProbPrevObs * Sur * MaxTrans;
            MaxAlpha = std::max(MaxAlpha, MaxResult);
        }
        return Obs * MaxAlpha;
    }
}

// ─────────────────────────────────────
double MDP::Markov(MacroState &StateJ, int CurrentState, int j, int T, int bufferIndex) {
    if (T == 0) {
        return StateJ.Obs[bufferIndex] * StateJ.InitProb;
    } else {
        double Obs = StateJ.Obs[bufferIndex];
        double MaxAlpha = -std::numeric_limits<double>::infinity();
        for (int i = CurrentState; i <= j; i++) {
            if (i >= 0) {
                int prevIndex = (bufferIndex - 1 + m_BufferSize) % m_BufferSize;
                double Value = GetTransProbability(i, j) * m_States[i].Forward[prevIndex];
                MaxAlpha = std::max(MaxAlpha, Value);
            }
        }
        return Obs * MaxAlpha;
    }
}

// ─────────────────────────────────────
double CalculateEntropy(const std::vector<double> &probs) {
    double entropy = 0.0;
    for (double prob : probs) {
        if (prob > 0) { // Avoid log(0) which is undefined
            entropy -= prob * log(prob);
        }
    }
    return entropy;
}

// ─────────────────────────────────────
int MDP::Inference(int CurrentState, int MaxState, int T) {
    double MaxValue = -std::numeric_limits<double>::infinity();
    int BestState = CurrentState;
    int bufferIndex = T % m_BufferSize;

    for (int j = CurrentState; j <= MaxState; j++) {
        if ((j < 0) || ((size_t)j >= m_States.size()))
            continue;
        MacroState &StateJ = m_States[j];
        // for (AudioState &SubState : StateJ.SubStates) {
        // if (SubState.Markov == MARKOV) {
        //     int StateSize = StateJ.SubStates.size();
        //
        //     // SubState.Forward[bufferIndex] = Markov(SubState, CurrentState, j, T, bufferIndex);
        // }
        // }

        if (StateJ.Markov == SEMIMARKOV) {
            StateJ.Forward[bufferIndex] = SemiMarkov(StateJ, CurrentState, j, T, bufferIndex);
        } else if (StateJ.Markov == MARKOV) {
            StateJ.Forward[bufferIndex] = Markov(StateJ, CurrentState, j, T, bufferIndex);
        }
    }

    // Sum
    double SumForward = 0;
    for (int j = CurrentState; j <= MaxState; j++) {
        if ((j < 0) || ((size_t)j >= m_States.size()))
            continue;
        SumForward += m_States[j].Forward[bufferIndex];
    }

    // Normalization
    std::vector<double> Probs;
    for (int j = CurrentState; j <= MaxState; j++) {
        if ((j < 0) || ((size_t)j >= m_States.size()))
            continue;

        MacroState &StateJ = m_States[j];
        if (T != 0) {
            double Forward = StateJ.Forward[bufferIndex];
            StateJ.Forward[bufferIndex] = Forward / SumForward;
        }
        if (StateJ.Forward[bufferIndex] > MaxValue) {
            MaxValue = StateJ.Forward[bufferIndex];
            BestState = j;
        }
        Probs.push_back(StateJ.Forward[bufferIndex]);
    }

    // TODO: Implement Cuvillier (2016)
    double Entropy = CalculateEntropy(Probs);
    double maxEntropy = log(Probs.size());
    double Conf = 1.0 - (Entropy / maxEntropy);
    // printf("config: %d, %f, %f\n", BestState, entropy, confidence);

    if (m_MinEntropy > 0) {
        if (Conf > m_MinEntropy) {
            return BestState;
        } else {
            return CurrentState;
        }
    }
    return BestState;
}

// ─────────────────────────────────────
int MDP::GetEvent(Description &Desc) {
    m_Desc = Desc;
    m_MaxScoreState = GetMaxJIndex(m_CurrentStateIndex);
    GetAudioObservations(m_CurrentStateIndex - 1, m_MaxScoreState, m_Tau);

    if (Desc.Silence || (size_t)m_CurrentStateIndex == m_States.size()) {
        if (m_CurrentStateIndex == -1) {
            return 0;
        }
        return m_States[m_CurrentStateIndex].ScorePos;
    }

    if (m_Tau == 0) {
        std::vector<double> InitialProb = GetInitialDistribution();
        for (int j = m_CurrentStateIndex; j < m_MaxScoreState; j++) {
            if (j < 0) {
                continue;
            }
            MacroState &StateJ = m_States[j];
            StateJ.InitProb = InitialProb[j - m_CurrentStateIndex];
        }
    }

    int StateIndex = Inference(m_CurrentStateIndex, m_MaxScoreState, m_Tau);
    if (StateIndex == -1) {
        return 0;
    }
    m_PsiN = UpdatePsiN(StateIndex); // Time Update

    // Return Score Position
    if (m_CurrentStateIndex == StateIndex) {
        m_CurrentStateIndex = StateIndex;
        return m_States[StateIndex].ScorePos;
    } else {
        m_CurrentStateIndex = StateIndex;

        // Config
        m_MinEntropy = m_States[StateIndex].Entropy;
        m_SyncStrength = m_States[StateIndex].SyncStrength;
        m_PhaseCoupling = m_States[StateIndex].PhaseCoupling;

        return m_States[StateIndex].ScorePos;
    }
}
} // namespace OScofo
