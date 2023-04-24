// Sub Expressions
const ftype HK0 = powf(pd, 2) + powf(pe, 2) + powf(pn, 2);
const ftype HK1 = powf(HK0, -1.0F/2.0F);
const ftype HK2 = P[7][9]*pn + P[8][9]*pe + P[9][9]*pd;
const ftype HK3 = 1.0F/HK0;
const ftype HK4 = P[7][8]*pn + P[8][8]*pe + P[8][9]*pd;
const ftype HK5 = P[7][7]*pn + P[7][8]*pe + P[7][9]*pd;
const ftype HK6 = HK1/(HK2*HK3*pd + HK3*HK4*pe + HK3*HK5*pn + R_TOF);


// Observation Jacobians
Hfusion[0] = 0;
Hfusion[1] = 0;
Hfusion[2] = 0;
Hfusion[3] = 0;
Hfusion[4] = 0;
Hfusion[5] = 0;
Hfusion[6] = 0;
Hfusion[7] = HK1*pn;
Hfusion[8] = HK1*pe;
Hfusion[9] = HK1*pd;
Hfusion[10] = 0;
Hfusion[11] = 0;
Hfusion[12] = 0;
Hfusion[13] = 0;
Hfusion[14] = 0;
Hfusion[15] = 0;
Hfusion[16] = 0;
Hfusion[17] = 0;
Hfusion[18] = 0;
Hfusion[19] = 0;
Hfusion[20] = 0;
Hfusion[21] = 0;
Hfusion[22] = 0;
Hfusion[23] = 0;


// Kalman gains
Kfusion[0] = HK6*(P[0][7]*pn + P[0][8]*pe + P[0][9]*pd);
Kfusion[1] = HK6*(P[1][7]*pn + P[1][8]*pe + P[1][9]*pd);
Kfusion[2] = HK6*(P[2][7]*pn + P[2][8]*pe + P[2][9]*pd);
Kfusion[3] = HK6*(P[3][7]*pn + P[3][8]*pe + P[3][9]*pd);
Kfusion[4] = HK6*(P[4][7]*pn + P[4][8]*pe + P[4][9]*pd);
Kfusion[5] = HK6*(P[5][7]*pn + P[5][8]*pe + P[5][9]*pd);
Kfusion[6] = HK6*(P[6][7]*pn + P[6][8]*pe + P[6][9]*pd);
Kfusion[7] = HK5*HK6;
Kfusion[8] = HK4*HK6;
Kfusion[9] = HK2*HK6;
Kfusion[10] = HK6*(P[7][10]*pn + P[8][10]*pe + P[9][10]*pd);
Kfusion[11] = HK6*(P[7][11]*pn + P[8][11]*pe + P[9][11]*pd);
Kfusion[12] = HK6*(P[7][12]*pn + P[8][12]*pe + P[9][12]*pd);
Kfusion[13] = HK6*(P[7][13]*pn + P[8][13]*pe + P[9][13]*pd);
Kfusion[14] = HK6*(P[7][14]*pn + P[8][14]*pe + P[9][14]*pd);
Kfusion[15] = HK6*(P[7][15]*pn + P[8][15]*pe + P[9][15]*pd);
Kfusion[16] = HK6*(P[7][16]*pn + P[8][16]*pe + P[9][16]*pd);
Kfusion[17] = HK6*(P[7][17]*pn + P[8][17]*pe + P[9][17]*pd);
Kfusion[18] = HK6*(P[7][18]*pn + P[8][18]*pe + P[9][18]*pd);
Kfusion[19] = HK6*(P[7][19]*pn + P[8][19]*pe + P[9][19]*pd);
Kfusion[20] = HK6*(P[7][20]*pn + P[8][20]*pe + P[9][20]*pd);
Kfusion[21] = HK6*(P[7][21]*pn + P[8][21]*pe + P[9][21]*pd);
Kfusion[22] = HK6*(P[7][22]*pn + P[8][22]*pe + P[9][22]*pd);
Kfusion[23] = HK6*(P[7][23]*pn + P[8][23]*pe + P[9][23]*pd);


