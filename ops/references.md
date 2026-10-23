# References

The papers behind ALTAIR's research-round code, with where each one is used.
What was found when checking each one is in `ops/research-ideas.md`.

## Implemented or checked

| Source | Used in | Notes |
|---|---|---|
| P. A. Forsyth, "A Hamilton Jacobi Bellman approach to optimal trade execution", *Applied Numerical Mathematics* 61 (2011). [Preprint](https://cs.uwaterloo.ca/~paforsyt/optimal_trade.pdf) | `oms/hjb_execution.hpp` | research-ideas 2e |
| Lai Jien Weng, "Optimal Execution with Alpha Signals", Monash, 2026 (SSRN 6323159) | `oms/alpha_execution.hpp` | 2f; eq. 10 corrected |
| D. Verma, "Market Impact Models for Small Funds", Verma Research Capital, 2026 (SSRN 6301920) | `oms/execution_state.hpp` | 2g |
| VISTAS eprint 14517, ICBDS 2025, doi [10.1109/ICBDS67396.2025.11376874](https://doi.org/10.1109/ICBDS67396.2025.11376874) (LSTM vs Transformer) | none | Too little detail to reproduce |
| Shindhe & Gokhale, NIFTY 50 deep-learning and ensemble forecasting, *Cureus Journal of Business and Economics* 3(1), 2026 | `models/meta_learners.hpp` (`GokhaleStack`) | 2h; same-day high/low look-ahead |
| J. T. Manhire, "Predicting Stock Market Prices with Physical Laws", Texas A&M, 2017; accepted for the 13th Annual Econophysics Colloquium, University of Warsaw, July 2017 (PDF supplied by Smit) | `analytics/action_principle.hpp` | 2i |
| D. Orrell, "A Quantum Walk Model of Financial Options", *Wilmott* 2021: 62-69, doi [10.1002/wilm.10918](https://doi.org/10.1002/wilm.10918) (SSRN 3512481; PDF supplied by Smit) | `analytics/quantum_walk.hpp` | 2q |
| D. Orrell, "A Quantum Oscillator Model of Stock Markets", Systems Forecasting (SSRN 3941518; PDF supplied by Smit) | `analytics/quantum_oscillator.hpp`, `app/quantum_smile_main.cpp` | 2p |
| Vishwas, Usha & Sangeetha, "Artificial intelligence-based stock market price prediction: a review", *Mathematical Models in Engineering*, 2025, doi 10.21595/mme.2025.24861 | none | 2j; a review, nothing to build |
| Charithra C. M., I. T. Hawaldar, Vinish P., P. Pinto & C. Shetty, "Deep learning-based stock market forecasting: a comparative analysis of ANN, CNN, and LSTM", *Investment Management and Financial Innovations* 23(2), 2026, 219–234, doi [10.21511/imfi.23(2).2026.17](http://dx.doi.org/10.21511/imfi.23(2).2026.17), CC BY 4.0 | `analytics/forecast_metrics.hpp` | 2k |
| G. S. Atsalakis & K. P. Valavanis, "Forecasting stock market short-term trends using a neuro-fuzzy based methodology", *Expert Systems with Applications* 36(7), 2009, 10696–10707, doi 10.1016/j.eswa.2009.02.043 | `models/anfis.hpp` | 2l |
| J.-S. R. Jang, "ANFIS: adaptive-network-based fuzzy inference system", *IEEE Trans. Systems, Man, and Cybernetics* 23(3), 1993 | `models/anfis.hpp` | The hybrid learning rule |
| N. Bawa, "Gamma Scalping and the Volatility Risk Premium", Medium, 2026-04-01 | `analytics/gamma_scalping.hpp` | Core formula as supplied by Smit (page not fetchable); research-ideas 2m |
| F. Ye et al., "Jane Street Stock prediction model based on LightGBM", ICSP 2021, 385–388, doi 10.1109/ICSP51882.2021.9408851 | none | Full text unavailable; 2m |
| M. Kamal & E. Derman, "When you cannot hedge continuously: the corrections to Black-Scholes", *Risk* 12(1), 1999 | `analytics/gamma_scalping.hpp` | Hedging error |
| H. E. Leland, "Option pricing and replication with transactions costs", *J. Finance* 40(5), 1985 | `analytics/gamma_scalping.hpp` | Cost-adjusted vol |
| K. Verma & A. K. Jha, "Predicting Nifty 50 Trends Using a Markov Chain Approach", *IJCRT* 12(12), 2024 (SSRN 5063966) | `models/tests/test_markov_nifty.cpp` (uses `models/markov.hpp`) | 2n |
| J. Olmo & M. Sanso-Navarro, "Forecasting the Performance of Hedge Fund Styles", 2012 (SSRN 1932225) | `analytics/stochastic_dominance.hpp` | 2o |
| G. Amisano & R. Giacomini, "Comparing density forecasts via weighted likelihood ratio tests", *J. Business & Economic Statistics* 25(2), 2007 | none | Cited by Olmo & Sanso-Navarro; not built |
| F. X. Diebold & R. S. Mariano, "Comparing predictive accuracy", *J. Business & Economic Statistics* 13(3), 1995 | `diebold_mariano` | |
| D. Harvey, S. Leybourne & P. Newbold, "Testing the equality of prediction mean squared errors", *Int. J. Forecasting* 13(2), 1997 | `diebold_mariano` | The small-sample factor |
| M. H. Pesaran & A. Timmermann, "A simple nonparametric test of predictive performance", *J. Business & Economic Statistics* 10(4), 1992 | `pesaran_timmermann` | |

The book *Quantitative Finance and Risk Management: A Physicist's Approach*
(J. W. Dash, World Scientific, ISBN 9814571237) was requested but could not
be accessed. Nothing in the code draws on it.

## The Charithra et al. (2026) reference list

The full list as the publisher's page gives it (54 entries; 17 and 18 are the
same paper). The ones that bear on ALTAIR's own method are marked **bold**:
Fischer & Krauss, the random-walk benchmark (Moosa & Burns), accuracy
measures (Hyndman & Koehler, Chai & Draxler), the direction test (Pesaran &
Timmermann), the statistics-vs-ML comparison (Makridakis et al.) and
Atsalakis & Valavanis.

1. **Atsalakis, G. S., & Valavanis, K. P. (2009). Forecasting stock market short-term trends using a neuro-fuzzy based methodology. *Expert Systems with Applications*, 36(7), 10696–10707.**
2. Bao, W., Cao, Y., Yang, Y., Che, H., Huang, J., & Wen, S. (2025). Data-driven stock forecasting models based on neural networks: A review. *Information Fusion*, 113, 102616.
3. Bao, W., Yue, J., & Rao, Y. (2017). A deep learning framework for financial time series using stacked autoencoders and long-short term memory. *PLOS ONE*, 12(7), e0180944.
4. Barua, M., Kumar, T., Raj, K., & Roy, A. M. (2024). Comparative analysis of deep learning models for stock price prediction in the Indian market. *FinTech*, 3(4), 551–568.
5. Bollerslev, T. (1986). Generalized autoregressive conditional heteroskedasticity. *Journal of Econometrics*, 31(3), 307–327.
6. Box, G. E. P., & Jenkins, G. M. (1976). *Time Series Analysis: Forecasting and Control* (2nd ed.). Holden-Day.
7. C M, C., Sharma, M., & Vikas, B. (2024). Relationship between the volume of trade and volatility spillover between the selected developed and emerging Asian markets. *Educational Administration: Theory and Practices*.
8. **Chai, T., & Draxler, R. R. (2014). Root mean square error (RMSE) or mean absolute error (MAE)? *Geoscientific Model Development*, 7(3), 1247–1250.**
9. Chatfield, C. (2000). *Time-Series Forecasting*. Chapman and Hall/CRC.
10. de Myttenaere, A., Golden, B., Le Grand, B., & Rossi, F. (2016). Mean absolute percentage error for regression models. *Neurocomputing*, 192, 38–48.
11. Engle, R. F. (1982). Autoregressive conditional heteroscedasticity with estimates of the variance of United Kingdom inflation. *Econometrica*, 50(4), 987.
12. Engle, R., & Mezrich, J. (1996). GARCH for groups. *Risk*, 9(8), 36–40.
13. Fama, E. F. (1991). Efficient capital markets: II. *The Journal of Finance*, 46(5), 1575–1617.
14. **Fischer, T., & Krauss, C. (2018). Deep learning with long short-term memory networks for financial market predictions. *European Journal of Operational Research*, 270(2), 654–669.**
15. Giantsidi, S., & Tarantola, C. (2025). Deep learning for financial forecasting: A review of recent trends. *International Review of Economics & Finance*, 104, 104719.
16. Heaton, J. (2018). Review of Goodfellow, Bengio & Courville, *Deep Learning*. *Genetic Programming and Evolvable Machines*, 19(1–2), 305–307.
17. Hochreiter, S., & Schmidhuber, J. (1997). Long short-term memory. *Neural Computation*, 9(8), 1735–1780.
18. (Duplicate of 17.)
19. Huang, W., Nakamori, Y., & Wang, S.-Y. (2005). Forecasting stock market movement direction with support vector machine. *Computers & Operations Research*, 32(10), 2513–2522.
20. **Hyndman, R. J., & Koehler, A. B. (2006). Another look at measures of forecast accuracy. *International Journal of Forecasting*, 22(4), 679–688.**
21. Kara, Y., Acar Boyacioglu, M., & Baykan, Ö. K. (2011). Predicting direction of stock price index movement using artificial neural networks and support vector machines. *Expert Systems with Applications*, 38(5), 5311–5319.
22. Kim, H. Y., & Won, C. H. (2018). Forecasting the volatility of stock price index: A hybrid model integrating LSTM with multiple GARCH-type models. *Expert Systems with Applications*, 103, 25–37.
23. Kim, K. (2003). Financial time series forecasting using support vector machines. *Neurocomputing*, 55(1–2), 307–319.
24. Kim, S., & Kim, H. (2016). A new metric of absolute percentage error for intermittent demand forecasts. *International Journal of Forecasting*, 32(3), 669–679.
25. Kim, T., & Kim, H. Y. (2019). Forecasting stock prices with a feature fusion LSTM-CNN model using different representations of the same data. *PLOS ONE*, 14(2), e0212320.
26. Kumar Meher, B., Thonse Hawaldar, I., Spulbar, C., & Birau, R. (2021). Forecasting stock market prices using mixed ARIMA model: a case study of Indian pharmaceutical companies. *Investment Management and Financial Innovations*, 18(1), 42–54.
27. Kumaria, A., Rajkar, A., Raut, A., & Nair, R. S. (2023). Forecasting the Indian financial markets with LSTM and price indicators (pp. 395–403). Springer.
28. Leung, Y., Mei, C.-L., & Zhang, W.-X. (2000). Statistical tests for spatial nonstationarity based on the geographically weighted regression model. *Environment and Planning A*, 32(1), 9–32.
29. Li, Y., & Ma, W. (2010). Applications of artificial neural networks in financial economics: A survey. *ISCID 2010*, 211–214.
30. Lo, A. W. (2004). The adaptive markets hypothesis. *The Journal of Portfolio Management*, 30(5), 15–29.
31. **Makridakis, S., Spiliotis, E., & Assimakopoulos, V. (2018). Statistical and machine learning forecasting methods: Concerns and ways forward. *PLOS ONE*, 13(3), e0194889.**
32. Mienye, I. D., & Swart, T. G. (2024). A comprehensive review of deep learning: Architectures, recent advances, and applications. *Information*, 15(12), 755.
33. **Moosa, I., & Burns, K. (2016). The random walk as a forecasting benchmark: drift or no drift? *Applied Economics*, 48(43), 4131–4142.**
34. Nelson, D. B. (1991). Conditional heteroskedasticity in asset returns: A new approach. *Econometrica*, 59(2), 347.
35. Nelson, D. M. Q., Pereira, A. C. M., & de Oliveira, R. A. (2017). Stock market's price movement prediction with LSTM neural networks. *IJCNN 2017*, 1419–1426.
36. Patel, J., Shah, S., Thakkar, P., & Kotecha, K. (2015). Predicting stock and stock price index movement using trend deterministic data preparation and machine learning techniques. *Expert Systems with Applications*, 42(1), 259–268.
37. **Pesaran, M. H., & Timmermann, A. (1992). A simple nonparametric test of predictive performance. *Journal of Business & Economic Statistics*, 10(4), 461.**
38. Poon, S.-H., & Granger, C. W. J. (2003). Forecasting volatility in financial markets: A review. *Journal of Economic Literature*, 41(2), 478–539.
39. Semenoglou, A.-A., Spiliotis, E., & Assimakopoulos, V. (2023). Image-based time series forecasting: A deep convolutional neural network approach. *Neural Networks*, 157, 39–53.
40. Sezer, O. B., Gudelek, M. U., & Ozbayoglu, A. M. (2020). Financial time series forecasting with deep learning: A systematic literature review: 2005–2019. *Applied Soft Computing*, 90, 106181.
41. Shiller, R. J. (2003). From efficient markets theory to behavioral finance. *Journal of Economic Perspectives*, 17(1), 83–104.
42. Srivastava, N., Hinton, G., Krizhevsky, A., Sutskever, I., & Salakhutdinov, R. (2014). Dropout: A simple way to prevent neural networks from overfitting. *JMLR*, 15(56), 1929–1958.
43. Thanh Noi, P., & Kappas, M. (2017). Comparison of random forest, k-nearest neighbor, and support vector machine classifiers for land cover classification using Sentinel-2 imagery. *Sensors*, 18(1), 18.
44. Tsantekidis, A., Passalis, N., Tefas, A., Kanniainen, J., Gabbouj, M., & Iosifidis, A. (2017). Using deep learning to detect price change indications in financial markets. *EUSIPCO 2017*, 2511–2515.
45. Tsay, R. S. (2010). *Analysis of Financial Time Series*. Wiley.
46. Vamossy, D. F. (2021). Investor emotions and earnings announcements. *Journal of Behavioral and Experimental Finance*, 30, 100474.
47. Vasenska, I. (2025). Comparative analysis of machine learning and deep learning models for tourism demand forecasting with economic indicators. *FinTech*, 4(3), 46.
48. Vivas, E., Allende-Cid, H., & Salas, R. (2020). A systematic review of statistical and machine learning methods for electrical power forecasting with reported MAPE score. *Entropy*, 22(12), 1412.
49. World Bank Group. (2023). *Global Economic Prospects*.
50. Yang, J., Zhang, L., Chen, C., Li, Y., Li, R., Wang, G., Jiang, S., & Zeng, Z. (2020). A hierarchical deep convolutional neural network and gated recurrent unit framework for structural damage detection. *Information Sciences*, 540, 117–130.
51. Zhang, G. P. (2003). Time series forecasting using a hybrid ARIMA and neural network model. *Neurocomputing*, 50, 159–175.
52. Zhang, G., Eddy Patuwo, B., & Hu, M. Y. (1998). Forecasting with artificial neural networks: The state of the art. *International Journal of Forecasting*, 14(1), 35–62.
53. Zhang, M.-L., & Zhou, Z.-H. (2007). ML-KNN: A lazy learning approach to multi-label learning. *Pattern Recognition*, 40(7), 2038–2048.
54. Zhang, X., Liang, X., Zhiyuli, A., Zhang, S., Xu, R., & Wu, B. (2019). AT-LSTM: An attention-based LSTM model for financial time series prediction. *IOP Conf. Series: Materials Science and Engineering*, 569(5), 052037.

The Atsalakis & Valavanis reference list was not readable from the sources
reached (the ScienceDirect page refuses automated access).
